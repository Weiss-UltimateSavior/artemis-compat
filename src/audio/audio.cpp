// Artemis compat audio backend.
//   Android: OpenSL ES — one SL player object per active voice; ogg decoded
//            with stb_vorbis (to short PCM), whole-buffer fed via a
//            SimpleBufferQueue; a callback re-enqueues when loop is set.
//   Host:   silent stub (links cleanly; `artc drive` stays quiet).
#include "audio/audio.h"

#include <map>
#include <vector>
#include <cmath>
#include <cstdlib>

#include "log/logger.h"
#include "pack/pack_manager.h"

#if defined(__ANDROID__)
#define STB_VORBIS_NO_STDIO 1
#endif
#include "../third_party/stb_vorbis/stb_vorbis.c"

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
void Audio::Stop(const std::string &) {}
void Audio::StopAll() {}
void Audio::SetVolume(const std::string &, int) {}
void Audio::PauseAll() {}
void Audio::ResumeAll() {}

#else // __ANDROID__ ---------------------------------------------------------

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

struct Audio::Impl {
    PackManager *packs = nullptr;

    SLObjectItf engine_obj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf output_mix = nullptr;

    struct Voice {
        SLObjectItf obj = nullptr;
        SLPlayItf play = nullptr;
        SLAndroidSimpleBufferQueueItf bq = nullptr;
        short *pcm = nullptr;
        SLuint32 pcm_bytes = 0;
        bool loop = false;
        int vol = 1000;
    };
    std::map<std::string, Voice> voices;
    bool engine_ok = false;
};

Audio::Audio() : impl_(new Impl) {}
Audio::~Audio() { Shutdown(); delete impl_; }

static void voice_callback(SLAndroidSimpleBufferQueueItf bq, void *ctx) {
    auto *v = static_cast<Audio::Impl::Voice *>(ctx);
    if (v && v->loop) (*bq)->Enqueue(bq, v->pcm, v->pcm_bytes);
}

static int volume_to_mb(int vol1000) {
    if (vol1000 <= 0) return -2200;
    const double db = 20.0 * std::log10(vol1000 / 1000.0);
    return static_cast<int>(db * 100.0);
}

void Audio::Init(PackManager *packs) {
    impl_->packs = packs;
    if (impl_->engine_ok) return;
    if (slCreateEngine(&impl_->engine_obj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS)
        return;
    (*impl_->engine_obj)->Realize(impl_->engine_obj, SL_BOOLEAN_FALSE);
    (*impl_->engine_obj)->GetInterface(impl_->engine_obj, SL_IID_ENGINE, &impl_->engine);
    if (impl_->engine &&
        (*impl_->engine)->CreateOutputMix(impl_->engine, &impl_->output_mix, 0, nullptr, nullptr) ==
            SL_RESULT_SUCCESS) {
        (*impl_->output_mix)->Realize(impl_->output_mix, SL_BOOLEAN_FALSE);
        impl_->engine_ok = true;
        Log(kLogInfo, "audio: OpenSL ES ready");
    }
}

void Audio::Shutdown() {
    StopAll();
    if (impl_->output_mix) { (*impl_->output_mix)->Destroy(impl_->output_mix); impl_->output_mix = nullptr; }
    if (impl_->engine_obj) { (*impl_->engine_obj)->Destroy(impl_->engine_obj); impl_->engine_obj = nullptr; }
    impl_->engine = nullptr;
    impl_->engine_ok = false;
}

bool Audio::Play(const std::string &key, const std::string &file, bool loop, int vol) {
    if (!impl_->engine) return false;
    std::vector<uint8_t> ogg;
    if (!impl_->packs || !impl_->packs->Read(file, ogg) || ogg.empty()) {
        Log(kLogWarn, "audio: ogg not found: " + file);
        return false;
    }
    int channels = 0, sample_rate = 0;
    short *pcm = nullptr;
    if (!stb_vorbis_decode_memory(ogg.data(), static_cast<int>(ogg.size()),
                                  &channels, &sample_rate, &pcm) || !pcm) {
        Log(kLogWarn, "audio: vorbis decode failed: " + file);
        return false;
    }
    long smp = 0;
    {   // exact sample count for the queue buffer length
        int err = 0;
        stb_vorbis *vv = stb_vorbis_open_memory(ogg.data(), (int)ogg.size(), &err, nullptr);
        if (vv) { smp = (long)stb_vorbis_stream_length_in_samples(vv); stb_vorbis_close(vv); }
    }
    if (smp <= 0) smp = 1024;
    const SLuint32 pcm_bytes = (SLuint32)(smp * 2 * channels);
    if (pcm_bytes > (1u << 26)) { Log(kLogWarn, "audio: file too large to queue: " + file); free(pcm); return false; }

    Stop(key);   // replace an existing voice on this key
    Impl::Voice v;
    v.pcm = pcm;
    v.pcm_bytes = pcm_bytes;
    v.loop = loop;
    v.vol = vol < 0 ? 0 : (vol > 1000 ? 1000 : vol);

    SLDataLocator_AndroidSimpleBufferQueue loc_bufq = { SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2 };
    const SLuint32 rate = (SLuint32)(sample_rate) * 1000;
    const SLuint32 chmask = channels == 1 ? SL_SPEAKER_FRONT_CENTER
                                          : (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT);
    SLDataFormat_PCM fmt = { SL_DATAFORMAT_PCM, (SLuint32)channels, rate,
                             SL_PCMSAMPLEFORMAT_FIXED_16, SL_PCMSAMPLEFORMAT_FIXED_16,
                             chmask, SL_BYTEORDER_LITTLEENDIAN };
    SLDataSource src = { &loc_bufq, &fmt };
    SLDataLocator_OutputMix loc_out = { SL_DATALOCATOR_OUTPUTMIX, impl_->output_mix };
    SLDataSink sink = { &loc_out, nullptr };

    SLObjectItf obj = nullptr;
    const SLInterfaceID ids[] = { SL_IID_ANDROIDSIMPLEBUFFERQUEUE, SL_IID_VOLUME };
    const SLboolean req[] = { SL_BOOLEAN_TRUE, SL_BOOLEAN_TRUE };
    SLresult r = (*impl_->engine)->CreateAudioPlayer(impl_->engine, &obj, &src, &sink,
                                                     2, ids, req);
    (void)r;
    if (!obj) { Log(kLogError, "audio: CreateAudioPlayer failed: " + file); free(pcm); return false; }
    (*obj)->Realize(obj, SL_BOOLEAN_FALSE);

    SLAndroidSimpleBufferQueueItf bq = nullptr;
    (*obj)->GetInterface(obj, SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &bq);
    if (!bq) { (*obj)->Destroy(obj); free(pcm); return false; }
    SLPlayItf play = nullptr;
    (*obj)->GetInterface(obj, SL_IID_PLAY, &play);

    v.obj = obj;
    v.play = play;
    v.bq = bq;
    impl_->voices[key] = v;                 // stable address for the callback ctx
    Impl::Voice *vv2 = &impl_->voices[key];
    (*bq)->RegisterCallback(bq, voice_callback, vv2);
    (*bq)->Enqueue(bq, vv2->pcm, vv2->pcm_bytes);

    SLVolumeItf volItf = nullptr;
    if ((*obj)->GetInterface(obj, SL_IID_VOLUME, &volItf) == SL_RESULT_SUCCESS && volItf)
        (*volItf)->SetVolumeLevel(volItf, volume_to_mb(vv2->vol));
    if (play) (*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING);

    Log(kLogInfo, "audio: play " + key + " " + file + " [" + std::to_string(channels) +
                      "ch " + std::to_string(sample_rate) + "Hz " +
                      std::to_string(pcm_bytes >> 10) + "KiB] loop=" + std::to_string(loop) +
                      " vol=" + std::to_string(vol));
    return true;
}

void Audio::Stop(const std::string &key) {
    auto it = impl_->voices.find(key);
    if (it == impl_->voices.end()) return;
    if (it->second.play) { (*it->second.play)->SetPlayState(it->second.play, SL_PLAYSTATE_STOPPED); }
    if (it->second.obj) { (*it->second.obj)->Destroy(it->second.obj); }
    if (it->second.pcm) free(it->second.pcm);
    impl_->voices.erase(it);
}
void Audio::StopAll() {
    for (auto &kv : impl_->voices) {
        if (kv.second.play) (*kv.second.play)->SetPlayState(kv.second.play, SL_PLAYSTATE_STOPPED);
        if (kv.second.obj) (*kv.second.obj)->Destroy(kv.second.obj);
        if (kv.second.pcm) free(kv.second.pcm);
    }
    impl_->voices.clear();
}
void Audio::SetVolume(const std::string &key, int vol) {
    auto it = impl_->voices.find(key);
    if (it == impl_->voices.end()) return;
    it->second.vol = vol < 0 ? 0 : (vol > 1000 ? 1000 : vol);
    SLVolumeItf vi = nullptr;
    if ((*it->second.obj)->GetInterface(it->second.obj, SL_IID_VOLUME, &vi) == SL_RESULT_SUCCESS && vi)
        (*vi)->SetVolumeLevel(vi, volume_to_mb(it->second.vol));
}
void Audio::PauseAll() {
    for (auto &kv : impl_->voices)
        if (kv.second.play) (*kv.second.play)->SetPlayState(kv.second.play, SL_PLAYSTATE_PAUSED);
}
void Audio::ResumeAll() {
    for (auto &kv : impl_->voices)
        if (kv.second.play) (*kv.second.play)->SetPlayState(kv.second.play, SL_PLAYSTATE_PLAYING);
}

#endif // __ANDROID__

} // namespace artc