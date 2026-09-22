// engine_context.h — the engine graph's single assembly point and owner.
//
// Historically every host (jni native_activity / jni DebugBridge / mac host /
// CLI) built its own subset of {PackManager, Ini, Audio, AudioChannels,
// Compositor, LuaEngine, AsbRunner} with slightly different order, and the
// LuaEngine constructed its own Audio (ownership inversion). This class is now
// the only place that creates the graph; hosts hold one EngineContext and use
// the accessors.
//
// Two-phase hosts (mac / Android create their GL window before the first
// frame) call Open() up front — pack chain, ini, audio, stage size — then
// Start() + BootFramework() once the GL context is current.
//
// Layering: core/ sits beside host/ — it assembles script/render/audio/pack.
// No engine module may include core/ (hosts only).
#pragma once
#include "config/ini.h"

#include <memory>
#include <string>
#include <vector>

namespace artc {

class PackManager;
class Audio;
class AudioChannels;
class Compositor;
class LuaEngine;
class AsbRunner;

class EngineContext {
public:
    EngineContext();
    ~EngineContext();
    EngineContext(const EngineContext &) = delete;
    EngineContext &operator=(const EngineContext &) = delete;

    // Phase 1 (no GL): resolve the pack chain (a directory holding root.pfs /
    // *.pfs, or a direct .pfs path), parse system.ini, create + initialize
    // audio, and read the stage size. Idempotent: a second call keeps the
    // already-open chain until Shutdown().
    bool Open(const std::string &data_dir, const std::string &os_id,
              const std::vector<uint8_t> &explicit_key = {});

    // Embedded hosts may use a writable application directory independently
    // of the pack location. Empty retains the official host's sidecar layout.
    bool Open(const std::string &data_dir, const std::string &os_id,
              const std::vector<uint8_t> &explicit_key, const std::string &save_dir);

    // Phase 2 (GL context must be current when `with_compositor`): compositor
    // Init + Lua session. Safe to call again after ResetSession() (compositor
    // GL is released and rebuilt, matching the old per-boot sequence).
    // `with_compositor=false` is the headless assembly used by the JNI
    // DebugBridge bootstrap, which has no GL context.
    bool Start(bool with_compositor = true);

    // Phase 3: create the AsbRunner, run system/first.iet and wire
    // jump/call/stop to it. Requires Start().
    // `drain_boot_queue` runs the CLI harness' historical post-boot drain:
    // queued boot *jumps* (the game_start entry) execute immediately, before
    // the handlers are installed; interactive hosts let their frame loop drain
    // instead.
    bool BootFramework(bool drain_boot_queue = false);

    // Open + Start + BootFramework, for hosts that boot in one shot.
    bool Boot(const std::string &data_dir, const std::string &os_id,
              const std::vector<uint8_t> &explicit_key = {});

    // Drop the Lua session + runner (keeps packs/ini/audio/compositor); used
    // by the GL-lost and [reset] re-boot paths.
    void ResetSession();

    // Full teardown (GL context must be current if a compositor was started).
    // Restores defaults so the same object can subsequently open another game.
    void Shutdown();

    bool Opened() const { return packs_ != nullptr; }
    bool Started() const { return lua_ != nullptr; }

    PackManager &packs() { return *packs_; }
    Ini &ini() { return ini_; }
    Audio &audio() { return *audio_; }
    AudioChannels &sounds() { return *sounds_; }
    Compositor &compositor() { return *compositor_; }
    LuaEngine &lua() { return *lua_; }
    AsbRunner &runner() { return *runner_; }

    int stageW() const { return stage_w_; }
    int stageH() const { return stage_h_; }
    const std::string &saveDir() const { return save_dir_; }
    const std::string &osId() const { return os_id_; }

    // Process-wide "current instance": lets the JNI shims (audio pause hooks,
    // DebugBridge host queries) reach whichever host started an engine,
    // without the old raw g_packs/g_lua globals.
    static EngineContext *Current();
    static void SetCurrent(EngineContext *ctx);

private:
    std::string ResolvePack(const std::string &data_dir) const;
    int StageInt(const char *key, int fallback) const;

    std::unique_ptr<PackManager> packs_;
    Ini ini_;
    std::unique_ptr<Audio> audio_;
    std::unique_ptr<AudioChannels> sounds_;
    std::unique_ptr<Compositor> compositor_;
    std::unique_ptr<LuaEngine> lua_;
    std::unique_ptr<AsbRunner> runner_;
    std::string os_id_ = "windows";
    std::string save_dir_;
    int stage_w_ = 1280;
    int stage_h_ = 720;
};

} // namespace artc
