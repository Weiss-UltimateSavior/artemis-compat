// Artemis compat — lightweight ogg-vorbis audio player.
// Android: OpenSL ES streaming player (one SL object per active voice).
// Host:   silent stub so the binary links and `artc drive` stays noise-free.
#ifndef ARTC_AUDIO_H
#define ARTC_AUDIO_H

#include <string>

namespace artc {

class PackManager;

class Audio {
public:
    struct Impl;
    Audio();
    ~Audio();

    // Create the backend (Android: OpenSL ES engine off the *global* devices).
    void Init(PackManager *packs);
    void Shutdown();

    // Decode `file` from the pack chain and play it under `key`.
    // vol1000 is the standard Artemis 0-1000 volume (used as linear gain).
    // loop lets the voice repeat until stopped.
    bool Play(const std::string &key, const std::string &file,
              bool loop, int vol1000);

    void Stop(const std::string &key);
    void StopAll();
    void SetVolume(const std::string &key, int vol1000);
    void PauseAll();
    void ResumeAll();

private:
    Impl *impl_;
};

} // namespace artc

#endif // ARTC_AUDIO_H