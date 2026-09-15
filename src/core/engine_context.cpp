// engine_context.cpp — see engine_context.h.
#include "core/engine_context.h"

#include "audio/audio.h"
#include "audio/audio_channels.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "render/compositor.h"
#include "script/asb_parser.h"
#include "script/iet_interpreter.h"
#include "script/lua_engine.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

namespace fs = std::filesystem;

namespace artc {
namespace {

std::string ToLower(std::string s) {
    for (char &c : s) c = static_cast<char>(std::tolower((unsigned char)c));
    return s;
}

bool EndsWith(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// See EngineContext::Current — registry only (hosts publish their instance).
EngineContext *g_current_ctx = nullptr;

} // namespace

EngineContext::EngineContext() = default;
EngineContext::~EngineContext() { Shutdown(); }

// A directory holding root.pfs (and its patch chain), or a direct .pfs path.
// root.pfs sorts first, matching every host's historical resolution.
std::string EngineContext::ResolvePack(const std::string &data_dir) const {
    if (!fs::is_directory(data_dir)) return data_dir;
    std::vector<std::string> packs;
    for (const auto &e : fs::directory_iterator(data_dir)) {
        if (e.is_regular_file() &&
            EndsWith(ToLower(e.path().filename().string()), ".pfs"))
            packs.push_back(e.path().string());
    }
    if (packs.empty()) return {};
    std::sort(packs.begin(), packs.end(), [](const std::string &a, const std::string &b) {
        const bool ra = EndsWith(ToLower(a), "/root.pfs");
        const bool rb = EndsWith(ToLower(b), "/root.pfs");
        if (ra != rb) return ra;
        return a < b;
    });
    return packs.front();
}

// Stage size lives in the ANDROID section on every tested title; WINDOWS is
// the historical fallback.
int EngineContext::StageInt(const char *key, int fallback) const {
    int v = ini_.GetInt("ANDROID", key, 0);
    if (v <= 0) v = ini_.GetInt("WINDOWS", key, 0);
    return v > 0 ? v : fallback;
}

bool EngineContext::Open(const std::string &data_dir, const std::string &os_id,
                         const std::vector<uint8_t> &explicit_key) {
    os_id_ = os_id;
    if (packs_) return true;  // pack chain is per-process
    const std::string pack = ResolvePack(data_dir);
    if (pack.empty()) {
        Log(kLogError, "engine: no .pfs pack at " + data_dir);
        return false;
    }
    save_dir_ = fs::path(pack).parent_path().string();
    packs_ = std::make_unique<PackManager>();
    if (!packs_->OpenChain(pack, explicit_key)) {
        Log(kLogError, "engine: cannot open pack chain: " + pack);
        packs_.reset();
        return false;
    }
    std::vector<uint8_t> ini_bytes;
    if (packs_->Read("system.ini", ini_bytes))
        ini_.Parse(std::string(ini_bytes.begin(), ini_bytes.end()));
    audio_ = std::make_unique<Audio>();
    audio_->Init(packs_.get());
    sounds_ = std::make_unique<AudioChannels>(*audio_);
    stage_w_ = std::max(1, StageInt("WIDTH", 1280));
    stage_h_ = std::max(1, StageInt("HEIGHT", 720));
    Log(kLogInfo, "engine: pack=" + pack + " os=" + os_id_ + " stage=" +
                      std::to_string(stage_w_) + "x" + std::to_string(stage_h_));
    return true;
}

bool EngineContext::Start(bool with_compositor) {
    if (!packs_) {
        Log(kLogError, "engine: Start() before Open()");
        return false;
    }
    if (with_compositor) {
        if (!compositor_) {
            compositor_ = std::make_unique<Compositor>();
            compositor_->SetPackManager(packs_.get());
        }
        compositor_->ReleaseGl();  // previous context (if any) is gone
        compositor_->Init(stage_w_, stage_h_);
    }

    lua_ = std::make_unique<LuaEngine>();
    lua_->SetSaveDir(save_dir_);
    if (!lua_->Init(packs_.get(), ini_, os_id_, stage_w_, stage_h_,
                    compositor_.get(), audio_.get(), sounds_.get())) {
        Log(kLogError, "engine: lua init failed");
        lua_.reset();
        return false;
    }
    return true;
}

bool EngineContext::BootFramework(bool drain_boot_queue) {
    if (!lua_) {
        Log(kLogError, "engine: BootFramework() before Start()");
        return false;
    }
    runner_ = std::make_unique<AsbRunner>();
    runner_->SetPackSource(packs_.get());

    IetRunner iet(packs_.get(), lua_.get());
    if (!iet.Run("system/first.iet")) {
        Log(kLogError, "engine: system/first.iet missing; boot aborted");
        return false;
    }
    // CLI harness order: drain the boot jumps before the frame-loop handlers
    // exist (the estag03 "call" is user-triggered and is not queued here).
    if (drain_boot_queue) {
        while (lua_->HasQueuedTag()) {
            std::string name;
            std::vector<std::pair<std::string, std::string>> attrs;
            if (!lua_->PopQueuedTag(&name, &attrs)) break;
            if (name == "jump") {
                std::string file, label;
                for (const auto &kv : attrs) {
                    if (kv.first == "file") file = kv.second;
                    else if (kv.first == "label") label = kv.second;
                }
                Log(kLogInfo, "boot queued [" + name + "] file=" + file +
                                  " label=" + label);
                runner_->Jump(file, label);
            } else {
                lua_->DispatchTag(name, attrs, false);
            }
        }
    }

    lua_->SetScriptRunner(runner_.get());
    AsbRunner *r = runner_.get();
    lua_->SetJumpHandler([r](const std::string &file, const std::string &label) {
        r->Jump(file, label);
    });
    lua_->SetCallHandler([r](const std::string &file, const std::string &label) {
        r->Call(file, label);
    });
    lua_->SetStopHandler([r](const std::string &tag) {
        // [stop] halts the script until an explicit jump/call re-enters it;
        // [return] pops the call frame.
        if (tag == "stop") {
            r->Halt();
            return;
        }
        if (!r->Return()) r->Halt();
    });
    Log(kLogInfo, "engine: boot sequence finished");
    return true;
}

bool EngineContext::Boot(const std::string &data_dir, const std::string &os_id,
                         const std::vector<uint8_t> &explicit_key) {
    return Open(data_dir, os_id, explicit_key) && Start() && BootFramework();
}

void EngineContext::ResetSession() {
    runner_.reset();
    lua_.reset();
}

void EngineContext::Shutdown() {
    if (Current() == this) SetCurrent(nullptr);
    ResetSession();
    if (compositor_) {
        compositor_->Shutdown();
        compositor_.reset();
    }
    sounds_.reset();
    if (audio_) {
        audio_->Shutdown();
        audio_.reset();
    }
    packs_.reset();
}

// Registry only (not ownership): hosts that boot an engine publish it here so
// JNI shims can reach it. Cleared by Shutdown().
EngineContext *EngineContext::Current() { return g_current_ctx; }

void EngineContext::SetCurrent(EngineContext *ctx) { g_current_ctx = ctx; }

} // namespace artc
