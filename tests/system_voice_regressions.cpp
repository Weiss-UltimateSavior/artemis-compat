#include "audio/audio.h"
#include "audio/audio_channels.h"
#include "config/ini.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>

namespace {
void Check(bool ok, const char *why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
class Output : public artc::Audio {
public:
    std::map<std::string, std::string> voices;
    int attempts = 0;
    bool Play(const std::string &key, const std::string &file, bool, int) override {
        ++attempts;
        if (file == "missing") return false;
        voices[key] = file;
        return true;
    }
    void Stop(const std::string &key) override { voices.erase(key); }
    bool IsPlaying(const std::string &key) const override { return voices.count(key) != 0; }
    void SetVolume(const std::string &, int) override {}
    void SetPan(const std::string &, int) override {}
};
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
        ("artc_sysvo_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    // These are opaque file fixtures, not a proposed commercial CSV schema.
    { std::ofstream f(root / "present.csv"); f << "synthetic data"; }
    { std::ofstream f(root / "empty.csv"); }
    Output output;
    artc::AudioChannels channels(output);
    artc::PackManager packs;
    artc::LuaEngine script;
    script.SetSaveDir(root.string());
    Check(script.Init(&packs, artc::Ini{}, "android", 1280, 720, nullptr, &output, &channels),
          "initialize system voice bridge");
    Check(script.DoString(R"(
        assert(user_func_sysvo == nil and type(conf) == 'table')
        e:setMagicPath{'catalog', 'present.csv'}
        e:setMagicPath{'voice', 'voice'}
        generic_calls=0; custom_calls=0
        function sysvo_dispatch(e,p)
            local hook = _G['user_func_sysvo']
            if hook then return hook(e,p) end
            local bytes = e:file(p.catalog)
            if not bytes or bytes == '' then return end
            generic_calls = generic_calls + 1
            e:tag{p.tag, id='sysvo', file=p.file, loop=0}
        end
        function invalid_catalog(e,p)
            local bytes = e:file(p.catalog)
            error('synthetic framework catalog failure: '..tostring(bytes))
        end
    )", "system-voice fixture"), "do not mask the generic framework branch");
    Check(script.CallEvent("sysvo_dispatch", {{"catalog",":catalog"},{"tag","seplay"},{"file",":voice/title"}}, false),
          "generic system voice calls the existing SE tag");
    Check(output.attempts == 1 && output.voices.size() == 1 &&
          output.voices.begin()->second == "voice/title", "system voice path resolves before audio playback");
    Check(script.CallEvent("sysvo_dispatch", {{"catalog",":catalog"},{"tag","voplay"},{"file",":voice/confirm"}}, false),
          "generic system voice also supports the voice tag");
    Check(output.attempts == 2 && output.voices.size() == 1 &&
          output.voices.begin()->second == "voice/confirm", "same channel replaces the system voice");
    for (int i=0; i<4; ++i) {
        Check(script.CallEvent("sysvo_dispatch", {{"catalog","absent.csv"},{"tag","seplay"},{"file","unused"}}, false),
              "framework can safely decline a missing catalog");
        Check(script.CallEvent("sysvo_dispatch", {{"catalog","empty.csv"},{"tag","seplay"},{"file","unused"}}, false),
              "framework can safely decline an empty catalog");
    }
    Check(output.attempts == 2, "missing and empty catalog never invent a playback request");
    Check(script.CallEvent("sysvo_dispatch", {{"catalog",":catalog"},{"tag","seplay"},{"file","missing"}}, false),
          "missing audio remains a failed playback rather than a Lua crash");
    Check(output.attempts == 3 && output.voices.size() == 1 &&
          output.voices.begin()->second == "voice/confirm", "failed replacement preserves the existing voice");
    Check(script.DoString(R"(
        function user_func_sysvo(e,p)
            custom_calls=custom_calls+1
            assert(p.file == 'custom')
        end
    )", "custom system voice hook"), "game may install its own system voice callback");
    Check(script.CallEvent("sysvo_dispatch", {{"catalog","absent.csv"},{"tag","seplay"},{"file","custom"}}, false),
          "custom callback takes precedence over generic file lookup");
    Check(output.attempts == 3 && script.DoString("assert(custom_calls==1 and generic_calls==3); user_func_sysvo=nil",
          "hook counters"), "engine neither overrides nor duplicates a custom hook");
    const int top = lua_gettop(script.state());
    for (int i=0; i<4; ++i) {
        Check(!script.CallEvent("invalid_catalog", {{"catalog","absent.csv"}}, false),
              "an unguarded framework catalog error is reported");
        Check(lua_gettop(script.state()) == top, "repeated callback errors do not leak the Lua stack");
    }
    Check(script.CallEvent("sysvo_dispatch", {{"catalog",":catalog"},{"tag","seplay"},{"file",":voice/after_error"}}, false),
          "a later voice callback still runs after framework errors");
    Check(output.attempts == 4 && output.voices.begin()->second == "voice/after_error",
          "traceback handling does not permanently silence audio");
    std::filesystem::remove_all(root);
    std::cout << "system voice regressions passed\n";
    return 0;
}
