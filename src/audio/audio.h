// Artemis compat — lightweight ogg-vorbis audio player.
// Android: OpenSL ES streaming player (one SL object per active voice).
// Host:   silent stub so the binary links and `artc drive` stays noise-free.
#ifndef ARTC_AUDIO_H
#define ARTC_AUDIO_H

#include <string>
#include <memory>
#include "audio/pcm_stream.h"

namespace artc {

class PackManager;

class Audio {
public:
    struct Impl;
    Audio();
    virtual ~Audio();

    // Create the backend (Android: OpenSL ES engine off the *global* devices).
    void Init(PackManager *packs);
    void Shutdown();

    // Decode `file` from the pack chain and play it under `key`.
    // vol1000 is the standard Artemis 0-1000 volume (used as linear gain).
    // loop lets the voice repeat until stopped.
    virtual bool Play(const std::string &key, const std::string &file,
              bool loop, int vol1000);
    virtual bool PlayStream(const std::string& key, std::unique_ptr<PcmStream> source, int vol1000);
    // Presented position, excluding PCM buffered ahead of the speaker.
    // Negative means the backend has no presentation clock (silent hosts).
    virtual double PlaybackMs(const std::string& key) const;

    virtual void Stop(const std::string &key);
    void StopAll();
    // KrKr2-Next: true while the voice under `key` is still producing audio
    // (looping voices never finish). Drives [wait se=] and setonsoundfinish.
    virtual bool IsPlaying(const std::string &key) const;
    virtual void SetVolume(const std::string &key, int vol1000);
    virtual void SetPan(const std::string &key, int pan1000);
    void PauseAll();
    void ResumeAll();

private:
    Impl *impl_;
};

} // namespace artc

#endif // ARTC_AUDIO_H
