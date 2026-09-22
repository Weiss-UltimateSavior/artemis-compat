// Artemis compat audio backend.
//   Android: OpenSL ES double-buffered output for the shared Vorbis stream.
//   Host:   silent stub (links cleanly; `artc drive` stays quiet).
#include "audio/audio.h"
#include "audio/pcm_ring.h"
#include "audio/vorbis_stream.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <chrono>
#include <condition_variable>
#include <map>
#include <algorithm>
#include <thread>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "log/logger.h"
#include "pack/pack_manager.h"

#if defined(ARTC_AUDIO_OPENSL)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

namespace artc {

// --------------------------------------------------------------------------
// Host stub ----------------------------------------------------------------
// --------------------------------------------------------------------------
#if !defined(ARTC_AUDIO_OPENSL)

struct Audio::Impl { PackManager *packs = nullptr; };

Audio::Audio() : impl_(new Impl) {}
Audio::~Audio() { impl_->packs = nullptr; delete impl_; }
void Audio::Init(PackManager *p) { impl_->packs = p; }
void Audio::Shutdown() {}
bool Audio::Play(const std::string &key, const std::string &file, bool loop, int) {
    Log(kLogInfo, "audio(host): play " + key + " " + file + " loop=" +
                      std::to_string(loop));
    return impl_->packs != nullptr;
}
bool Audio::PlayStream(const std::string&, std::unique_ptr<PcmStream>, int) { return false; }
double Audio::PlaybackMs(const std::string&) const { return -1; }
void Audio::Stop(const std::string &) {}
void Audio::StopAll() {}
bool Audio::IsPlaying(const std::string &) const { return false; }
void Audio::SetVolume(const std::string &, int) {}
void Audio::SetPan(const std::string &, int) {}
void Audio::PauseAll() {}
void Audio::ResumeAll() {}

#else // __ANDROID__ ---------------------------------------------------------

// Decode-ahead design (T2-3): one feed thread decodes every voice's Vorbis
// stream into its SPSC ring; the OpenSL buffer-queue callback only copies
// ring → buffer (silence on underrun). See audio/pcm_ring.h.
struct Audio::Impl {
    PackManager* packs = nullptr;
    SLObjectItf engine_obj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf output_mix = nullptr;
    bool paused = false;

    // 2048 stereo frames per OpenSL buffer (matches the 4096-sample buffers);
    // the ring holds 8 chunks ≈ 0.37 s at 44.1 kHz, absorbing decode jitter.
    static constexpr size_t kChunkSamples = 4096;
    static constexpr size_t kRingSamples = kChunkSamples * 8;

    struct Voice {
        SLObjectItf obj = nullptr;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf bq = nullptr;
        std::unique_ptr<PcmStream> source;
        std::array<std::array<int16_t, 4096>, 2> buffers{};
        size_t next_buffer = 0;
        std::atomic<int> pan{0};
        std::mutex mutex;
        PcmRing ring{kRingSamples};
        // Shared with Impl so the feed thread can report underruns even after
        // this voice is stopped.
        std::atomic<uint64_t> *underruns = nullptr;
        ~Voice() {
            // Destroy waits for any running buffer callback before the
            // compressed data, decoder and queue buffers are released.
            if (obj) (*obj)->Destroy(obj);
        }
        // Producer side: decode until the ring is full or the stream ends.
        // Only the feed thread (or the one-shot prime in PlayStream, before
        // the voice is published) may call this.
        size_t Feed() {
            int16_t scratch[kChunkSamples];
            size_t produced = 0;
            while (ring.Space() >= kChunkSamples && !source->Ended()) {
                const size_t frames = source->ReadStereo(scratch, kChunkSamples / 2);
                if (!frames) break;
                ring.Write(scratch, frames * 2);
                produced += frames * 2;
            }
            return produced;
        }
        // Consumer side (OpenSL callback): pure memory copy + silence padding.
        bool Queue() {
            auto& pcm = buffers[next_buffer];
            const size_t frames = pcm.size() / 2;
            const size_t got = ring.Read(pcm.data(), pcm.size()) / 2;
            if (got < frames) {
                std::fill(pcm.begin() + got * 2, pcm.end(), 0);
                if (source->Ended()) {
                    // Stream finished and its tail is drained: stop queuing so
                    // the queue count reaches zero (IsPlaying → false).
                    if (got == 0) return false;
                } else if (underruns) {
                    underruns->fetch_add(1, std::memory_order_relaxed);
                }
            }
            ApplyStereoPan(pcm.data(), frames, pan.load(std::memory_order_relaxed));
            if ((*bq)->Enqueue(bq, pcm.data(), static_cast<SLuint32>(frames * 4)) != SL_RESULT_SUCCESS)
                return false;
            next_buffer = (next_buffer + 1) % buffers.size();
            return true;
        }
    };

    std::mutex voices_mutex;
    std::map<std::string, std::shared_ptr<Voice>> voices;
    std::atomic<uint64_t> underruns{0};   // cumulative, for the feed log
    std::thread feed;
    std::atomic<bool> feed_running{false};
    std::mutex feed_mutex;
    std::condition_variable feed_cv;

    std::shared_ptr<Voice> FindVoice(const std::string& key) {
        std::lock_guard<std::mutex> lk(voices_mutex);
        const auto it = voices.find(key);
        return it == voices.end() ? nullptr : it->second;
    }

    void FeedLoop() {
        uint64_t last_log = 0;
        while (feed_running.load(std::memory_order_relaxed)) {
            std::vector<std::shared_ptr<Voice>> snapshot;
            {
                std::lock_guard<std::mutex> lk(voices_mutex);
                snapshot.reserve(voices.size());
                for (const auto& kv : voices) snapshot.push_back(kv.second);
            }
            bool produced = false;
            for (const auto& v : snapshot)
                if (v->Feed()) produced = true;
            if (!produced) {
                std::unique_lock<std::mutex> lk(feed_mutex);
                feed_cv.wait_for(lk, std::chrono::milliseconds(5));
            }
            // Underrun report (callback pulled silence): 5s cadence keeps it
            // off the hot path while still surfacing real playback hiccups.
            const uint64_t now = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            if (now - last_log >= 5000) {
                last_log = now;
                const uint64_t total = underruns.load(std::memory_order_relaxed);
                if (total) Log(kLogWarn, "audio: underruns=" + std::to_string(total));
            }
        }
    }
};

Audio::Audio() : impl_(new Impl) {}
Audio::~Audio() { Shutdown(); delete impl_; }

static void voice_callback(SLAndroidSimpleBufferQueueItf, void* ctx) {
    auto* v = static_cast<Audio::Impl::Voice*>(ctx);
    std::lock_guard<std::mutex> lock(v->mutex);
    v->Queue();
}

static int volume_to_mb(int vol1000) {
    if (vol1000 <= 0) return SL_MILLIBEL_MIN;
    return static_cast<int>(2000.0 * std::log10(std::min(vol1000, 1000) / 1000.0));
}

void Audio::Init(PackManager* packs) {
    impl_->packs = packs;
    if (impl_->engine) return;
    if (slCreateEngine(&impl_->engine_obj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*impl_->engine_obj)->Realize(impl_->engine_obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    if ((*impl_->engine_obj)->GetInterface(impl_->engine_obj, SL_IID_ENGINE, &impl_->engine) != SL_RESULT_SUCCESS) return;
    if ((*impl_->engine)->CreateOutputMix(impl_->engine, &impl_->output_mix, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) return;
    if ((*impl_->output_mix)->Realize(impl_->output_mix, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) return;
    // Decode-ahead thread: all decoding leaves the callback thread.
    impl_->feed_running = true;
    impl_->feed = std::thread([this] { impl_->FeedLoop(); });
    Log(kLogInfo, "audio: OpenSL ES ready (decode-ahead feed thread)");
}

void Audio::Shutdown() {
    if (impl_->feed_running.exchange(false)) {
        impl_->feed_cv.notify_all();
        if (impl_->feed.joinable()) impl_->feed.join();
    }
    StopAll();
    if (impl_->output_mix) { (*impl_->output_mix)->Destroy(impl_->output_mix); impl_->output_mix = nullptr; }
    if (impl_->engine_obj) { (*impl_->engine_obj)->Destroy(impl_->engine_obj); impl_->engine_obj = nullptr; }
    impl_->engine = nullptr;
    impl_->packs = nullptr;
}

bool Audio::Play(const std::string& key, const std::string& file, bool loop, int vol) {
    if (!impl_->engine || !impl_->output_mix || !impl_->packs) return false;
    auto source=std::make_unique<VorbisStream>();
    if (!source->Open([this](const std::string& name, std::vector<uint8_t>& bytes) {
            return impl_->packs->Read(name, bytes);
        }, file, loop)) return false;
    return PlayStream(key,std::move(source),vol);
}

bool Audio::PlayStream(const std::string& key, std::unique_ptr<PcmStream> pcm, int vol) {
    if (!impl_->engine || !impl_->output_mix || !pcm) return false;
    auto v = std::make_shared<Impl::Voice>();
    v->source=std::move(pcm);
    SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM format = {SL_DATAFORMAT_PCM, 2, static_cast<SLuint32>(v->source->SampleRate()) * 1000,
        SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
        SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource source = {&loc, &format};
    SLDataLocator_OutputMix output = {SL_DATALOCATOR_OUTPUTMIX, impl_->output_mix};
    SLDataSink sink = {&output, nullptr};
    const SLInterfaceID ids[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean required[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE};
    if ((*impl_->engine)->CreateAudioPlayer(impl_->engine, &v->obj, &source, &sink, 2, ids, required) != SL_RESULT_SUCCESS)
        return false;
    if ((*v->obj)->Realize(v->obj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS ||
        (*v->obj)->GetInterface(v->obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &v->bq) != SL_RESULT_SUCCESS ||
        (*v->obj)->GetInterface(v->obj, SL_IID_PLAY, &v->play) != SL_RESULT_SUCCESS) return false;
    // Prime the ring before the voice is published: the voice is invisible to
    // the feed thread and no callback is registered yet, so this is a
    // race-free single-producer fill (no startup underrun).
    v->underruns = &impl_->underruns;
    v->Feed();
    (*v->bq)->RegisterCallback(v->bq, voice_callback, v.get());
    if (!v->Queue()) return false;   // both startup buffers are checked
    if (!v->Queue()) return false;
    SLVolumeItf gain = nullptr;
    if ((*v->obj)->GetInterface(v->obj, SL_IID_VOLUME, &gain) == SL_RESULT_SUCCESS)
        (*gain)->SetVolumeLevel(gain, volume_to_mb(vol));
    if (!impl_->paused && (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS)
        return false;
    {
        std::lock_guard<std::mutex> lk(impl_->voices_mutex);
        impl_->voices[key] = v;
    }
    impl_->feed_cv.notify_all();
    Log(kLogInfo, "audio: play stream " + key);
    return true;
}

double Audio::PlaybackMs(const std::string& key) const {
    const auto v = const_cast<Impl*>(impl_)->FindVoice(key);
    if (!v) return -1;
    SLmillisecond ms=0;
    return (*v->play)->GetPosition(v->play,&ms)==SL_RESULT_SUCCESS ? ms : -1;
}

void Audio::Stop(const std::string& key) {
    {
        std::lock_guard<std::mutex> lk(impl_->voices_mutex);
        impl_->voices.erase(key);
    }
    impl_->feed_cv.notify_all();
}
void Audio::StopAll() {
    {
        std::lock_guard<std::mutex> lk(impl_->voices_mutex);
        impl_->voices.clear();
    }
    impl_->feed_cv.notify_all();
}

bool Audio::IsPlaying(const std::string& key) const {
    const auto v = const_cast<Impl*>(impl_)->FindVoice(key);
    if (!v) return false;
    std::lock_guard<std::mutex> lock(v->mutex);
    SLAndroidSimpleBufferQueueState state{};
    if ((*v->bq)->GetState(v->bq, &state) != SL_RESULT_SUCCESS) return true;
    return state.count > 0;
}

void Audio::SetVolume(const std::string& key, int vol) {
    const auto v = impl_->FindVoice(key);
    if (!v) return;
    SLVolumeItf gain = nullptr;
    auto obj = v->obj;
    if ((*obj)->GetInterface(obj, SL_IID_VOLUME, &gain) == SL_RESULT_SUCCESS && gain)
        (*gain)->SetVolumeLevel(gain, volume_to_mb(vol));
}

void Audio::SetPan(const std::string& key, int pan) {
    const auto v = impl_->FindVoice(key);
    if (v) v->pan.store(std::clamp(pan, -1000, 1000));
}

void Audio::PauseAll() {
    impl_->paused = true;
    std::vector<std::shared_ptr<Impl::Voice>> snapshot;
    {
        std::lock_guard<std::mutex> lk(impl_->voices_mutex);
        for (const auto& kv : impl_->voices) snapshot.push_back(kv.second);
    }
    for (const auto& v : snapshot)
        (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PAUSED);
}
void Audio::ResumeAll() {
    impl_->paused = false;
    std::vector<std::shared_ptr<Impl::Voice>> snapshot;
    {
        std::lock_guard<std::mutex> lk(impl_->voices_mutex);
        for (const auto& kv : impl_->voices) snapshot.push_back(kv.second);
    }
    for (const auto& v : snapshot)
        (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PLAYING);
    impl_->feed_cv.notify_all();
}

#endif // __ANDROID__
} // namespace artc
