#include "audio/audio.h"
#include "audio/audio_channels.h"
#include "config/ini.h"
#include "script/lua_engine.h"
#include <iostream>
#include <memory>

namespace {
class ObservedAudio : public artc::Audio {
public:
    explicit ObservedAudio(int &destroyed) : destroyed_(destroyed) {}
    ~ObservedAudio() override { ++destroyed_; }
private:
    int &destroyed_;
};
}

int main() {
    int destroyed = 0;
    {
        auto audio = std::make_unique<ObservedAudio>(destroyed);
        artc::AudioChannels sounds(*audio);
        // Multiple sessions borrow the same backend. Closing a session must
        // neither delete it nor leave the next session with dangling pointers.
        for (int i = 0; i < 3; ++i) {
            artc::LuaEngine lua;
            if (!lua.Init(nullptr, artc::Ini{}, "android", 1280, 720,
                          nullptr, audio.get(), &sounds)) return 1;
            lua.PauseAudio();
            lua.ResumeAudio();
        }
        if (destroyed != 0) return 2;
        sounds.Reset();
        audio->StopAll();
    }
    if (destroyed != 1) return 3;
    // The existing headless API permits omitting audio entirely.
    artc::LuaEngine silent;
    if (!silent.Init(nullptr, artc::Ini{}, "android", 1280, 720)) return 4;
    silent.PauseAudio();
    silent.ResumeAudio();
    std::cout << "audio ownership regressions passed\n";
}
