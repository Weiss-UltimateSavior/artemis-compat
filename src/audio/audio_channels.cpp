#include "audio/audio_channels.h"
#include <algorithm>
#include <cmath>

namespace artc {

float AudioChannels::Ramp::At(double now) const {
    const double t = duration <= 0 ? 1 : std::clamp((now - start) / duration, 0.0, 1.0);
    return from + (to - from) * static_cast<float>(t);
}

bool AudioChannels::Play(const std::string& channel, const std::string& file,
                         bool loop, int gain, int time, double now, bool crossfade) {
    Update(now);
    gain = std::clamp(gain, 0, 1000);
    time = std::max(time, 0);
    const std::string key = "track:" + std::to_string(++serial_);
    // Retain the current track if opening its replacement fails.
    if (!output_.Play(key, file, loop, time > 0 ? 0 : gain)) return false;
    Stop(channel, crossfade ? time : 0, now);
    Voice v;
    v.channel = channel;
    v.gain = {now, double(time), float(time > 0 ? 0 : gain), float(gain)};
    voices_[key] = v;
    current_[channel] = key;
    Update(now);
    return true;
}

void AudioChannels::Stop(const std::string& channel, int time, double now) {
    // Include retiring tracks so a stop during a second crossfade cannot
    // leave an older BGM audible in the background.
    for (auto it = voices_.begin(); it != voices_.end();) {
        auto& v = it->second;
        if (v.channel != channel) { ++it; continue; }
        if (time <= 0) {
            output_.Stop(it->first);
            it = voices_.erase(it);
        } else {
            v.gain = {now, double(time), v.gain.At(now), 0};
            v.stopping = true;
            ++it;
        }
    }
    if (time <= 0) current_.erase(channel);
}

void AudioChannels::Fade(const std::string& channel, int gain, int time, double now) {
    const auto c = current_.find(channel);
    if (c == current_.end()) return;
    auto v = voices_.find(c->second);
    if (v == voices_.end() || v->second.stopping) return;
    auto& ramp = v->second.gain;
    ramp = {now, double(std::max(time, 0)), ramp.At(now), float(std::clamp(gain, 0, 1000))};
    Update(now);
}

void AudioChannels::Pan(const std::string& channel, int pan, int time, double now) {
    const auto c = current_.find(channel);
    if (c == current_.end()) return;
    auto v = voices_.find(c->second);
    if (v == voices_.end()) return;
    auto& ramp = v->second.pan;
    ramp = {now, double(std::max(time, 0)), ramp.At(now), float(std::clamp(pan, -1000, 1000))};
    Update(now);
}

bool AudioChannels::IsPlaying(const std::string& channel) const {
    const auto it = current_.find(channel);
    return it != current_.end() && output_.IsPlaying(it->second);
}

void AudioChannels::Update(double now) {
    for (auto it = voices_.begin(); it != voices_.end();) {
        auto& v = it->second;
        if ((v.stopping && v.gain.Done(now)) || !output_.IsPlaying(it->first)) {
            output_.Stop(it->first); // retire system renderer and decoded data
            const auto c = current_.find(v.channel);
            if (c != current_.end() && c->second == it->first) current_.erase(c);
            it = voices_.erase(it);
            continue;
        }
        const int gain = static_cast<int>(std::lround(v.gain.At(now)));
        const int pan = static_cast<int>(std::lround(v.pan.At(now)));
        if (gain != v.last_gain) { output_.SetVolume(it->first, gain); v.last_gain = gain; }
        if (pan != v.last_pan) { output_.SetPan(it->first, pan); v.last_pan = pan; }
        ++it;
    }
}

} // namespace artc
