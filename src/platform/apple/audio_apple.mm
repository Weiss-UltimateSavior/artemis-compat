// audio_apple.mm — Apple (iOS / macOS) AudioUnit backend for the Artemis
// compat engine.
//
// Structure mirrors the OHAudio backend (src/platform/ohos/audio_ohos.cpp):
// one output AudioUnit per voice; Vorbis decoding and intro/loop sequencing
// stay in shared engine code, and the render callback only pulls the requested
// stereo block from the PcmStream (padding the tail with silence) plus pan and
// volume. Retired voices are queued to a releaser thread that stops the unit
// and waits for any in-flight render callback before disposing it, so no
// callback outlives its voice. Voices are never released on the render thread.
//
// PlaybackMs follows the OHAudio presentation-clock contract: the render
// callback publishes the sample time of the buffer it is rendering, and the
// unit keeps rendering (silence) after the stream ends, so the clock keeps
// advancing past the submitted frame count exactly like
// OH_AudioRenderer_GetTimestamp. A voice created while paused is never started
// and therefore reports no presentation clock (-1).
#include "audio/audio.h"
#include "audio/vorbis_stream.h"

#import <TargetConditionals.h>
#import <Foundation/Foundation.h>
#import <AudioToolbox/AudioToolbox.h>
#if TARGET_OS_IOS
#import <AVFoundation/AVFoundation.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "log/logger.h"
#include "pack/pack_manager.h"

namespace artc {

namespace {

struct Voice {
    AudioUnit unit = nullptr;
    std::unique_ptr<PcmStream> source;
    uint64_t total_frames = 0;
    std::string key;
    std::atomic<uint64_t> submitted_frames{0};
    std::atomic<int64_t> presented_frames{0};  // render-clock sample time, frames
    std::atomic<bool> finished{false};
    std::atomic<bool> started{false};
    std::mutex mutex;              // serializes the render callback vs disposal
    int sample_rate = 0;
    std::atomic<int> vol1000{1000};
    std::atomic<int> pan1000{0};
};

float VolumeToGain(int vol1000) {
    if (vol1000 <= 0) return 0.0f;
    if (vol1000 >= 1000) return 1.0f;
    return static_cast<float>(vol1000) / 1000.0f;
}

// Render callback: pull the requested stereo block from the stream, pad the
// tail with silence, then apply pan and volume. Once published, the voice's
// source is read only here (a single render thread per unit, like the OHAudio
// write-data callback).
OSStatus OnRender(void *inRefCon, AudioUnitRenderActionFlags *,
                  const AudioTimeStamp *inTimeStamp, UInt32, UInt32,
                  AudioBufferList *ioData) {
    auto *v = static_cast<Voice *>(inRefCon);
    if (!v || !ioData || ioData->mNumberBuffers < 1) return noErr;
    AudioBuffer &buf = ioData->mBuffers[0];
    if (!buf.mData || buf.mDataByteSize == 0) return noErr;
    if (inTimeStamp)
        v->presented_frames.store(static_cast<int64_t>(inTimeStamp->mSampleTime),
                                  std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(v->mutex);
    auto *out = static_cast<int16_t *>(buf.mData);
    const size_t frames = buf.mDataByteSize / (sizeof(int16_t) * 2);
    const size_t written = v->source->ReadStereo(out, frames);
    v->submitted_frames.fetch_add(written, std::memory_order_relaxed);
    const size_t bytes = written * sizeof(int16_t) * 2;
    if (bytes < buf.mDataByteSize)
        std::memset(static_cast<uint8_t *>(buf.mData) + bytes, 0,
                    buf.mDataByteSize - bytes);
    if (v->source->Ended()) v->finished = true;
    ApplyStereoPan(out, written, v->pan1000.load(std::memory_order_relaxed));
    const float gain = VolumeToGain(v->vol1000.load(std::memory_order_relaxed));
    if (gain < 1.0f) {
        for (size_t i = 0; i < written * 2; ++i)
            out[i] = static_cast<int16_t>(std::lround(static_cast<float>(out[i]) * gain));
    }
    return noErr;
}

// One output unit per voice in the stream's own sample rate (the I/O unit
// converts to the hardware route, like one OHAudio renderer per voice).
// iOS uses AURemoteIO; macOS uses AUHAL in output-only mode.
AudioUnit CreateVoiceUnit(int sample_rate, Voice *voice) {
    AudioComponentDescription desc{};
    desc.componentType = kAudioUnitType_Output;
    desc.componentManufacturer = kAudioUnitManufacturer_Apple;
#if TARGET_OS_IOS
    desc.componentSubType = kAudioUnitSubType_RemoteIO;
#else
    desc.componentSubType = kAudioUnitSubType_HALOutput;
#endif
    AudioComponent component = AudioComponentFindNext(nullptr, &desc);
    if (!component) {
        Log(kLogError, "audio: no output AudioUnit component found");
        return nullptr;
    }
    AudioUnit unit = nullptr;
    if (AudioComponentInstanceNew(component, &unit) != noErr || !unit) return nullptr;
#if !TARGET_OS_IOS
    // AUHAL on macOS: output only, never touch the microphone. The output
    // element is 0; the input element of an output unit is 1.
    const UInt32 enable = 1;
    const UInt32 disable = 0;
    if (AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Output, 0, &enable, sizeof(enable)) != noErr ||
        AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO,
                             kAudioUnitScope_Input, 1, &disable, sizeof(disable)) != noErr) {
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }
#endif
    // Client format: interleaved stereo signed 16-bit (PcmStream contract).
    AudioStreamBasicDescription fmt{};
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    fmt.mSampleRate = static_cast<Float64>(sample_rate);
    fmt.mChannelsPerFrame = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = fmt.mBytesPerPacket = sizeof(int16_t) * 2;
    fmt.mBitsPerChannel = 16;
    AURenderCallbackStruct cb{};
    cb.inputProc = OnRender;
    cb.inputProcRefCon = voice;
    if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr ||
        AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr ||
        AudioUnitInitialize(unit) != noErr) {
        AudioComponentInstanceDispose(unit);
        return nullptr;
    }
    return unit;
}

// Retire path: runs on the releaser thread. AudioOutputUnitStop only prevents
// *new* callbacks, so wait for an in-flight one to leave the source before the
// unit (and with it the source) is disposed.
void DestroyVoice(std::unique_ptr<Voice> &v) {
    if (!v) return;
    if (v->unit) {
        AudioOutputUnitStop(v->unit);
        {
            std::lock_guard<std::mutex> lk(v->mutex);
        }
        AudioUnitUninitialize(v->unit);
        AudioComponentInstanceDispose(v->unit);
        v->unit = nullptr;
    }
    v.reset();
}

} // namespace

struct Audio::Impl {
    PackManager *packs = nullptr;
    std::mutex mutex;
    std::map<std::string, std::unique_ptr<Voice>> voices;
    bool paused = false;
    std::mutex retire_mutex;
    std::condition_variable retire_ready;
    std::deque<std::unique_ptr<Voice>> retired;
    bool closing = false;
    std::thread releaser;

    Impl() : releaser([this] {
        for (;;) {
            std::unique_ptr<Voice> voice;
            {
                std::unique_lock<std::mutex> lock(retire_mutex);
                retire_ready.wait(lock, [this] { return closing || !retired.empty(); });
                if (retired.empty()) return;
                voice = std::move(retired.front());
                retired.pop_front();
            }
            // Stop and dispose are synchronous but still wait on the render
            // thread; keep that off the engine's render/input thread.
            DestroyVoice(voice);
        }
    }) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(retire_mutex);
            closing = true;
        }
        retire_ready.notify_one();
        releaser.join();  // every callback has ended before unloading the backend
    }

    void Retire(std::unique_ptr<Voice> &voice) {
        if (!voice) return;
        if (voice->unit) AudioOutputUnitStop(voice->unit);  // silence immediately
        {
            std::lock_guard<std::mutex> lock(retire_mutex);
            retired.push_back(std::move(voice));
        }
        retire_ready.notify_one();
    }
};

Audio::Audio() : impl_(new Impl) {}
Audio::~Audio() { Shutdown(); delete impl_; }

void Audio::Init(PackManager *packs) {
    impl_->packs = packs;
#if TARGET_OS_IOS
    // Best effort: a session failure is logged, never silently swallowed by a
    // backend abort. Hosts that need stricter routing own the session before
    // opening the engine; the units below still render on the default route.
    @autoreleasepool {
        NSError *err = nil;
        AVAudioSession *session = [AVAudioSession sharedInstance];
        if (![session setCategory:AVAudioSessionCategoryPlayback
                             mode:AVAudioSessionModeDefault
                            options:0
                            error:&err] || err) {
            Log(kLogWarn, std::string("audio: AVAudioSession setCategory failed: ") +
                              (err ? err.localizedDescription.UTF8String : "unknown"));
        }
        err = nil;
        if (![session setActive:YES error:&err] || err) {
            Log(kLogWarn, std::string("audio: AVAudioSession setActive failed: ") +
                              (err ? err.localizedDescription.UTF8String : "unknown"));
        }
    }
#endif
    Log(kLogInfo, "audio: Apple AudioUnit backend ready");
}

void Audio::Shutdown() {
    StopAll();
    impl_->packs = nullptr;
}

bool Audio::Play(const std::string &key, const std::string &file, bool loop, int vol) {
    auto source = std::make_unique<VorbisStream>();
    if (!impl_->packs || !source->Open(
            [this](const std::string& name, std::vector<uint8_t>& bytes) {
                return impl_->packs->Read(name, bytes);
            }, file, loop)) {
        Log(kLogWarn, "audio: cannot open Vorbis stream: " + file);
        return false;
    }
    return PlayStream(key, std::move(source), vol);
}

bool Audio::PlayStream(const std::string& key, std::unique_ptr<PcmStream> source, int vol) {
    if (!source) return false;
    auto voice = std::make_unique<Voice>();
    voice->source = std::move(source);
    const int sample_rate = voice->source->SampleRate();
    voice->total_frames = voice->source->FrameCount();
    voice->key = key;
    voice->sample_rate = sample_rate;
    voice->vol1000 = vol < 0 ? 0 : (vol > 1000 ? 1000 : vol);

    voice->unit = CreateVoiceUnit(sample_rate, voice.get());
    if (!voice->unit) {
        Log(kLogError, "audio: AudioUnit create failed: " + key);
        return false;
    }
    const uint64_t voice_frames = voice->total_frames;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        auto it = impl_->voices.find(key);
        if (it != impl_->voices.end()) {
            impl_->Retire(it->second);
            impl_->voices.erase(it);
        }
        if (!impl_->paused && AudioOutputUnitStart(voice->unit) != noErr) {
            Log(kLogError, "audio: AudioUnit start failed: " + key);
            impl_->Retire(voice);
            return false;
        }
        voice->started = !impl_->paused;
        impl_->voices[key] = std::move(voice);
    }

    Log(kLogInfo, "audio: play " + key + " [2ch " + std::to_string(sample_rate) + "Hz " +
                      std::to_string(voice_frames) + " frames] vol=" +
                      std::to_string(vol));
    return true;
}

double Audio::PlaybackMs(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = impl_->voices.find(key);
    if (it == impl_->voices.end() || !it->second) return -1;
    const Voice &v = *it->second;
    if (!v.started.load(std::memory_order_relaxed)) return -1;  // never rendered
    return 1000.0 * static_cast<double>(v.presented_frames.load(std::memory_order_relaxed)) /
           static_cast<double>(v.sample_rate);
}

void Audio::Stop(const std::string &key) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->voices.find(key);
    if (it == impl_->voices.end()) return;
    impl_->Retire(it->second);
    impl_->voices.erase(it);
}

bool Audio::IsPlaying(const std::string &key) const {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->voices.find(key);
    if (it == impl_->voices.end() || !it->second) return false;
    const Voice &v = *it->second;
    if (!v.finished.load()) return true;
    // The callback queues PCM ahead of the speaker: keep reporting playing
    // until the rendered sample time passes the submitted frame count.
    return static_cast<double>(v.presented_frames.load()) <
           static_cast<double>(v.submitted_frames.load());
}

void Audio::StopAll() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    for (auto &kv : impl_->voices) impl_->Retire(kv.second);
    impl_->voices.clear();
}

void Audio::SetVolume(const std::string &key, int vol) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    auto it = impl_->voices.find(key);
    if (it == impl_->voices.end() || !it->second) return;
    it->second->vol1000 = vol < 0 ? 0 : (vol > 1000 ? 1000 : vol);
}

void Audio::SetPan(const std::string &key, int pan) {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    const auto it = impl_->voices.find(key);
    if (it != impl_->voices.end() && it->second)
        it->second->pan1000.store(std::clamp(pan, -1000, 1000), std::memory_order_relaxed);
}

void Audio::PauseAll() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->paused = true;
    for (auto &kv : impl_->voices)
        if (kv.second && kv.second->unit && kv.second->started.load())
            AudioOutputUnitStop(kv.second->unit);
}

void Audio::ResumeAll() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->paused = false;
    for (auto &kv : impl_->voices) {
        if (!kv.second || !kv.second->unit) continue;
        // Starting an already-running unit is a no-op; this also starts voices
        // that were created while paused (OHAudio resumes those too).
        if (AudioOutputUnitStart(kv.second->unit) == noErr)
            kv.second->started = true;
    }
}

} // namespace artc
