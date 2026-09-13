// Artemis compat audio backend.
//   Android: OpenSL ES double-buffered output for the shared Vorbis stream.
//   Host:   silent stub (links cleanly; `artc drive` stays quiet).
#include "audio/audio.h"
#include "audio/vorbis_stream.h"
#include <array>
#include <atomic>
#include <memory>
#include <mutex>

#include <map>
#include <algorithm>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "log/logger.h"
#include "pack/pack_manager.h"

#if defined(__ANDROID__)
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#endif

namespace artc {

// --------------------------------------------------------------------------
// Host stub ----------------------------------------------------------------
// --------------------------------------------------------------------------
#if !defined(__ANDROID__)

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

struct Audio::Impl {
    PackManager* packs = nullptr;
    SLObjectItf engine_obj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf output_mix = nullptr;
    bool paused = false;
    struct Voice {
        SLObjectItf obj = nullptr;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf bq = nullptr;
        std::unique_ptr<PcmStream> source;
        std::array<std::array<int16_t, 4096>, 2> buffers{};
        size_t next_buffer = 0;
        std::atomic<int> pan{0};
        std::mutex mutex;
        ~Voice() {
            // Destroy waits for any running buffer callback before the
            // compressed data, decoder and queue buffers are released.
            if (obj) (*obj)->Destroy(obj);
        }
        bool Queue() {
            auto& pcm = buffers[next_buffer];
            const size_t count = source->ReadStereo(pcm.data(), pcm.size() / 2);
            if (!count) return false;
            ApplyStereoPan(pcm.data(), count, pan.load(std::memory_order_relaxed));
            if ((*bq)->Enqueue(bq, pcm.data(), static_cast<SLuint32>(count * 4)) != SL_RESULT_SUCCESS)
                return false;
            next_buffer = (next_buffer + 1) % buffers.size();
            return true;
        }
    };
    std::map<std::string, std::unique_ptr<Voice>> voices;
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
    Log(kLogInfo, "audio: OpenSL ES ready");
}

void Audio::Shutdown() {
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
    auto v=std::make_unique<Impl::Voice>();
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
    (*v->bq)->RegisterCallback(v->bq, voice_callback, v.get());
    if (!v->Queue()) return false;
    v->Queue();
    SLVolumeItf gain = nullptr;
    if ((*v->obj)->GetInterface(v->obj, SL_IID_VOLUME, &gain) == SL_RESULT_SUCCESS)
        (*gain)->SetVolumeLevel(gain, volume_to_mb(vol));
    if (!impl_->paused && (*v->play)->SetPlayState(v->play, SL_PLAYSTATE_PLAYING) != SL_RESULT_SUCCESS)
        return false;
    impl_->voices[key] = std::move(v);
    Log(kLogInfo, "audio: play stream " + key);
    return true;
}

double Audio::PlaybackMs(const std::string& key) const {
    const auto it=impl_->voices.find(key);
    if(it==impl_->voices.end()) return -1;
    SLmillisecond ms=0;
    return (*it->second->play)->GetPosition(it->second->play,&ms)==SL_RESULT_SUCCESS ? ms : -1;
}

void Audio::Stop(const std::string& key) { impl_->voices.erase(key); }
void Audio::StopAll() { impl_->voices.clear(); }

bool Audio::IsPlaying(const std::string& key) const {
    const auto it = impl_->voices.find(key);
    if (it == impl_->voices.end()) return false;
    auto& v = *it->second;
    std::lock_guard<std::mutex> lock(v.mutex);
    SLAndroidSimpleBufferQueueState state{};
    if ((*v.bq)->GetState(v.bq, &state) != SL_RESULT_SUCCESS) return true;
    return state.count > 0;
}

void Audio::SetVolume(const std::string& key, int vol) {
    const auto it = impl_->voices.find(key);
    if (it == impl_->voices.end()) return;
    SLVolumeItf gain = nullptr;
    auto obj = it->second->obj;
    if ((*obj)->GetInterface(obj, SL_IID_VOLUME, &gain) == SL_RESULT_SUCCESS && gain)
        (*gain)->SetVolumeLevel(gain, volume_to_mb(vol));
}

void Audio::SetPan(const std::string& key, int pan) {
    const auto it = impl_->voices.find(key);
    if (it != impl_->voices.end()) it->second->pan.store(std::clamp(pan, -1000, 1000));
}

void Audio::PauseAll() {
    impl_->paused = true;
    for (const auto& kv : impl_->voices) (*kv.second->play)->SetPlayState(kv.second->play, SL_PLAYSTATE_PAUSED);
}
void Audio::ResumeAll() {
    impl_->paused = false;
    for (const auto& kv : impl_->voices) (*kv.second->play)->SetPlayState(kv.second->play, SL_PLAYSTATE_PLAYING);
}

#endif // __ANDROID__
} // namespace artc
