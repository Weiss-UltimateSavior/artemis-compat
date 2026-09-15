// lua_tags_audio.cpp — BGM / SE / voice playback tags.
//
// BGM uses one logical channel; SE/voice use numbered channels. The engine
// owns fades and crossfades; backends only output individual tracks.
// The 13 playback/stop/fade/pan names share one handler because their
// parameters (channel routing, time, gain) are computed from the same
// attribute set.
#include "script/lua_engine.h"
#include "audio/audio.h"
#include "audio/audio_channels.h"
#include "log/logger.h"
#include <algorithm>
#include <cstdlib>
#include <string>

namespace artc {

bool LuaEngine::TagAudio(const std::string &tag, TagAttrs &m) {
    if (!sounds_) return false;
    const bool bgm = tag == "splay" || tag == "sxfade" ||
                     tag == "sstop" || tag == "sfade" || tag == "span";
    const bool play = tag == "splay" || tag == "sxfade" ||
                      tag == "seplay" || tag == "voplay" ||
                      tag == "vbplay" || tag == "bplay" || tag == "s2play";
    const std::string channel = bgm ? "bgm" : (m.count("id") ? m.at("id") : "0");
    const int time = m.count("time") ? std::max(0, std::atoi(m.at("time").c_str())) : 0;
    const int gain = m.count("gain") ? std::atoi(m.at("gain").c_str()) :
                     (m.count("volume") ? std::atoi(m.at("volume").c_str()) : 1000);
    const double now = NowMs();
    if (play) {
        if (m.count("file") && !m.at("file").empty()) {
            const bool loop = m.count("loop") ? m.at("loop") == "1" : bgm;
            sounds_->Play(channel, ResolvePackPath(m.at("file")),
                           loop, gain, time, now, tag == "sxfade");
        }
        return true;
    }
    if (tag == "sstop" || tag == "sestop") {
        sounds_->Stop(channel, time, now);
        return true;
    }
    if (tag == "sfade" || tag == "sefade") {
        sounds_->Fade(channel, gain, time, now);
        return true;
    }
    if (tag == "span" || tag == "sepan") {
        sounds_->Pan(channel, std::atoi(m["pan"].c_str()), time, now);
        return true;
    }
    return true;
}

bool LuaEngine::TagAllSoundStopRaw(lua_State *L) {
    if (audio_) audio_->StopAll();
    return true;
}

} // namespace artc
