#pragma once

#include "audio/audio.h"
#include <cstdint>
#include <map>
#include <string>

namespace artc {

// Script channels and envelopes belong to the engine, independent of the
// output API. A replaced BGM may keep playing while its successor fades in.
class AudioChannels {
public:
    explicit AudioChannels(Audio& output) : output_(output) {}
    bool Play(const std::string& channel, const std::string& file, bool loop,
              int gain, int time_ms, double now_ms, bool crossfade = false);
    void Stop(const std::string& channel, int time_ms, double now_ms);
    void Fade(const std::string& channel, int gain, int time_ms, double now_ms);
    void Pan(const std::string& channel, int pan, int time_ms, double now_ms);
    bool IsPlaying(const std::string& channel) const;
    void Update(double now_ms);

private:
    struct Ramp {
        double start = 0, duration = 0;
        float from = 0, to = 0;
        float At(double now) const;
        bool Done(double now) const { return now >= start + duration; }
    };
    struct Voice {
        std::string channel;
        Ramp gain, pan;
        int last_gain = -1, last_pan = 2001;
        bool stopping = false;
    };
    Audio& output_;
    uint64_t serial_ = 0;
    std::map<std::string, Voice> voices_;
    std::map<std::string, std::string> current_;
};

} // namespace artc
