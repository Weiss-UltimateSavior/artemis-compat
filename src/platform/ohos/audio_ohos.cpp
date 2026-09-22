// audio_ohos.cpp — OpenHarmony OHAudio backend for the Artemis compat engine.
//
// One renderer per voice; Vorbis decoding and intro/loop sequencing are shared
// engine code. Callbacks decode only the requested stereo block. Renderers are
// stopped and released before their sources, so no callback outlives its voice.
#include "audio/audio.h"
#include "audio/vorbis_stream.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log/logger.h"
#include "pack/pack_manager.h"


#include <ohaudio/native_audiorenderer.h>
#include <ohaudio/native_audiostream_base.h>
#include <ohaudio/native_audiostreambuilder.h>

namespace artc {

namespace {

struct Voice {
    OH_AudioRenderer *renderer = nullptr;
    std::unique_ptr<PcmStream> source;
    uint64_t total_frames = 0;
    std::string key;
    std::atomic<uint64_t> submitted_frames{0};
    std::atomic<bool> finished{false};
    std::mutex mutex;              // serializes source callbacks
    int channels = 0;
    int sample_rate = 0;
    int vol1000 = 1000;
    std::atomic<int> pan1000{0};
};

OH_AudioData_Callback_Result OnWriteData(OH_AudioRenderer *, void *user_data,
                                         void *audio_data, int32_t size) {
    auto *v = static_cast<Voice *>(user_data);
    auto *out = static_cast<uint8_t *>(audio_data);
    if (!v || size <= 0) return AUDIO_DATA_CALLBACK_RESULT_VALID;
    std::lock_guard<std::mutex> lk(v->mutex);
    const size_t want = static_cast<size_t>(size);
    const size_t frames = want / (sizeof(int16_t) * 2);
    const size_t written = v->source->ReadStereo(reinterpret_cast<int16_t*>(out), frames);
    const size_t bytes = written * sizeof(int16_t) * 2;
    v->submitted_frames.fetch_add(written, std::memory_order_relaxed);
    if (bytes < want) std::memset(out + bytes, 0, want - bytes);
    if (v->source->Ended()) v->finished = true;
    ApplyStereoPan(reinterpret_cast<int16_t*>(out), written,
                   v->pan1000.load(std::memory_order_relaxed));
    return AUDIO_DATA_CALLBACK_RESULT_VALID;
}

float VolumeToGain(int vol1000) {
    if (vol1000 <= 0) return 0.0f;
    if (vol1000 >= 1000) return 1.0f;
    return static_cast<float>(vol1000) / 1000.0f;
}

void DestroyVoice(std::unique_ptr<Voice> &v) {
    if (!v) return;
    if (v->renderer) {
        uint32_t underflows = 0;
        const auto stats = OH_AudioRenderer_GetUnderflowCount(v->renderer, &underflows);
        Log(kLogInfo, "audio: retire " + v->key + " submitted=" +
            std::to_string(v->submitted_frames.load()) + " underflows=" +
            (stats == AUDIOSTREAM_SUCCESS ? std::to_string(underflows) : "unavailable"));
        OH_AudioRenderer_Stop(v->renderer);
        OH_AudioRenderer_Release(v->renderer);
        v->renderer = nullptr;
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
            // Stop waits for queued audio (commonly >50 ms on OHAudio).
            // Keep that wait off the engine's render/input thread.
            DestroyVoice(voice);
        }
    }) {}

    ~Impl() {
        {
            std::lock_guard<std::mutex> lock(retire_mutex);
            closing = true;
        }
        retire_ready.notify_one();
        releaser.join(); // every callback has ended before unloading the engine
    }

    void Retire(std::unique_ptr<Voice>& voice) {
        if (!voice) return;
        if (voice->renderer) OH_AudioRenderer_SetVolume(voice->renderer, 0);
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
    Log(kLogInfo, "audio: OHAudio backend ready");
}

void Audio::Shutdown() {
    StopAll();
    impl_->packs = nullptr;
}

bool Audio::Play(const std::string &key, const std::string &file, bool loop, int vol) {
    auto source=std::make_unique<VorbisStream>();
    if (!impl_->packs || !source->Open(
            [this](const std::string& name, std::vector<uint8_t>& bytes) {
                return impl_->packs->Read(name, bytes);
            }, file, loop)) {
        Log(kLogWarn, "audio: cannot open Vorbis stream: " + file);
        return false;
    }
    return PlayStream(key,std::move(source),vol);
}

bool Audio::PlayStream(const std::string& key, std::unique_ptr<PcmStream> source, int vol) {
    if(!source) return false;
    auto voice=std::make_unique<Voice>();
    voice->source=std::move(source);
    const int sample_rate = voice->source->SampleRate();
    const int channels = 2;
    voice->total_frames = voice->source->FrameCount();
    voice->key = key;
    voice->channels = channels;
    voice->sample_rate = sample_rate;
    voice->vol1000 = vol < 0 ? 0 : (vol > 1000 ? 1000 : vol);

    OH_AudioStreamBuilder *builder = nullptr;
    if (OH_AudioStreamBuilder_Create(&builder, AUDIOSTREAM_TYPE_RENDERER) != AUDIOSTREAM_SUCCESS ||
        !builder) {
        Log(kLogError, "audio: OH_AudioStreamBuilder_Create failed: " + key);
        return false;
    }
    OH_AudioStreamBuilder_SetSamplingRate(builder, sample_rate);
    OH_AudioStreamBuilder_SetChannelCount(builder, channels);
    OH_AudioStreamBuilder_SetSampleFormat(builder, AUDIOSTREAM_SAMPLE_S16LE);
    OH_AudioStreamBuilder_SetEncodingType(builder, AUDIOSTREAM_ENCODING_TYPE_RAW);
    OH_AudioStreamBuilder_SetLatencyMode(builder, AUDIOSTREAM_LATENCY_MODE_NORMAL);
    OH_AudioStreamBuilder_SetRendererInfo(builder, AUDIOSTREAM_USAGE_GAME);
    OH_AudioStreamBuilder_SetRendererWriteDataCallback(builder, OnWriteData, voice.get());
    OH_AudioRenderer *renderer = nullptr;
    const OH_AudioStream_Result gen = OH_AudioStreamBuilder_GenerateRenderer(builder, &renderer);
    OH_AudioStreamBuilder_Destroy(builder);
    if (gen != AUDIOSTREAM_SUCCESS || !renderer) {
        Log(kLogError, "audio: GenerateRenderer failed (" + std::to_string(static_cast<int>(gen)) +
                           "): " + key);
        return false;
    }
    voice->renderer = renderer;
    OH_AudioRenderer_SetVolume(renderer, VolumeToGain(voice->vol1000));

    const uint64_t voice_frames = voice->total_frames;
    {
        std::lock_guard<std::mutex> lk(impl_->mutex);
        auto it = impl_->voices.find(key);
        if (it != impl_->voices.end()) {
            impl_->Retire(it->second);
            impl_->voices.erase(it);
        }
        if (!impl_->paused && OH_AudioRenderer_Start(renderer) != AUDIOSTREAM_SUCCESS) {
            Log(kLogError, "audio: renderer start failed: " + key);
            impl_->Retire(voice);
            return false;
        }
        impl_->voices[key] = std::move(voice);
    }

    Log(kLogInfo, "audio: play " + key + " " + key + " [" + std::to_string(channels) +
                      "ch " + std::to_string(sample_rate) + "Hz " +
                      std::to_string(voice_frames) + " frames]" +
                      " vol=" + std::to_string(vol));
    return true;
}

double Audio::PlaybackMs(const std::string& key) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it=impl_->voices.find(key);
    if(it==impl_->voices.end()) return -1;
    const auto& v=*it->second;
    int64_t frames=0, timestamp=0;
    if(OH_AudioRenderer_GetTimestamp(v.renderer,CLOCK_MONOTONIC,&frames,&timestamp)!=AUDIOSTREAM_SUCCESS) return -1;
    return 1000.0*frames/v.sample_rate;
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
    const auto& v = *it->second;
    if (!v.finished.load()) return true;
    // The callback queues PCM ahead of the speaker. Releasing its renderer
    // as soon as the last buffer is filled truncates the tail of the voice.
    int64_t position = 0, timestamp = 0;
    if (OH_AudioRenderer_GetTimestamp(v.renderer, CLOCK_MONOTONIC, &position, &timestamp)
            != AUDIOSTREAM_SUCCESS) return true;
    return position < static_cast<int64_t>(v.submitted_frames.load());
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
    if (it->second->renderer)
        OH_AudioRenderer_SetVolume(it->second->renderer, VolumeToGain(it->second->vol1000));
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
        if (kv.second && kv.second->renderer) OH_AudioRenderer_Pause(kv.second->renderer);
}

void Audio::ResumeAll() {
    std::lock_guard<std::mutex> lk(impl_->mutex);
    impl_->paused = false;
    for (auto &kv : impl_->voices)
        // Even when the callback has queued the last sample, the renderer
        // may still have an unplayed tail that was paused with the app.
        if (kv.second && kv.second->renderer)
            OH_AudioRenderer_Start(kv.second->renderer);
}

} // namespace artc
