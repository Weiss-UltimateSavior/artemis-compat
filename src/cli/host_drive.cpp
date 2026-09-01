// host_drive.cpp — `artc drive`: host frame-loop harness for the compat engine.
//
// The Android build runs the whole game in a native-engine frame loop
// (native_activity.cpp: boot first.iet → jump-handler → per-frame script
// stepping + input + onEnterFrame + composer present). That loop is what M3
// title interaction lives in, but it is only exercisable on a phone. This
// driver replicates the loop on the host with the *real* frame-loop code paths
// (same LuaEngine / AsbRunner / Compositor objects), replacing GL with the
// math-only host compositor. Injected taps drive the lyevent/button flow, so
// the title → click-wait → language/story sequence can be iterated against the
// DT pack without a device.
//
//   artc drive <pack> [--os android|windows] [--frames N] [--tap x,y@frame ...]
//
// A tap reproduces the Android touch model: mouse(point) + key 1 down/up +
// ClickAt, all on the requested frame.

#include "config/ini.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "render/compositor.h"
#include "script/asb_parser.h"
#include "script/iet_interpreter.h"
#include "script/lua_engine.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace artc {

struct DriveTap {
    int frame = 0;
    float x = 0, y = 0;
    float x2 = 0, y2 = 0;   // drag end point (when drag=true)
    bool drag = false;
};

static bool ParseTap(const std::string &s, DriveTap *out) {
    int f = 0;
    float x = 0, y = 0, x2 = 0, y2 = 0;
    // drag spec: x1,y1,x2,y2@frame
    if (std::sscanf(s.c_str(), "%f,%f,%f,%f@%d", &x, &y, &x2, &y2, &f) == 5) {
        out->frame = f;
        out->x = x; out->y = y;
        out->x2 = x2; out->y2 = y2;
        out->drag = true;
        return true;
    }
    if (std::sscanf(s.c_str(), "%f,%f@%d", &x, &y, &f) != 3) return false;
    out->frame = f;
    out->x = x;
    out->y = y;
    return true;
}

int RunDrive(const std::string &pack, const std::string &osName, int frames,
             const std::vector<std::string> &tapSpecs) {
    std::vector<DriveTap> taps;
    for (const std::string &t : tapSpecs) {
        DriveTap tap;
        if (!ParseTap(t, &tap)) {
            Log(kLogError, "bad --tap spec: " + t + " (want x,y@frame)");
            return 1;
        }
        taps.push_back(tap);
    }

    PackManager packs;
    if (!packs.OpenChain(pack, {})) {
        Log(kLogError, "OpenChain failed: " + pack);
        return 1;
    }

    Ini ini;
    std::vector<uint8_t> ini_bytes;
    if (packs.Read("system.ini", ini_bytes))
        ini.Parse(std::string(ini_bytes.begin(), ini_bytes.end()));
    // Stage resolution from the level default (kept in ANDROID on all titles);
    // the osName above only selects framework behavior, not the surface size.
    const int stage_w = ini.GetInt("ANDROID", "WIDTH", 1280);
    const int stage_h = ini.GetInt("ANDROID", "HEIGHT", 720);

    Log(kLogInfo, "drive: pack=" + pack + " os=" + osName + " stage=" +
                      std::to_string(stage_w) + "x" + std::to_string(stage_h) +
                      " frames=" + std::to_string(frames) +
                      " taps=" + std::to_string(taps.size()));

    // Save location = the game directory (next to the pack), like the engine
    // writes system.dat beside root.pfs.
    std::string save_dir = pack;
    const size_t slash = save_dir.find_last_of('/');
    if (slash != std::string::npos) save_dir = save_dir.substr(0, slash);
    Log(kLogInfo, "drive: save dir=" + save_dir);

    Compositor compositor;
    compositor.Init(stage_w, stage_h);

    // Boot state lives on the heap so a [reset] tag can tear it down and
    // re-run the boot chain (same as native_activity's window-loss path).
    std::unique_ptr<LuaEngine> lua;
    auto runner = std::make_unique<AsbRunner>();
    auto boot = [&]() {
        compositor.ReleaseGl();   // clear the previous frame's layers
        lua = std::make_unique<LuaEngine>();
        lua->SetSaveDir(save_dir);
        if (!lua->Init(&packs, ini, osName, stage_w, stage_h, &compositor)) {
            Log(kLogError, "lua init failed");
            return false;
        }
        IetRunner iet(&packs, lua.get());
        if (!iet.Run("system/first.iet")) {
            Log(kLogError, "first.iet missing; boot aborted");
            return false;
        }
        if (iet.Stopped()) Log(kLogInfo, "drive: boot script hit [stop]");
        runner = std::make_unique<AsbRunner>();
        runner->SetPackSource(&packs);
        // first.iet ends by enqueueing the estag03 chain (language select →
        // reset). Drain the boot *jumps* (game_start entry) right away; the
        // estag03 "call" is user-triggered and must stay queued so the
        // language click drives reset at the right time.
        while (lua->HasQueuedTag()) {
            std::string name;
            std::vector<std::pair<std::string, std::string>> attrs;
            if (!lua->PopQueuedTag(&name, &attrs)) break;
            if (name == "jump") {
                std::string file, label;
                for (const auto &kv : attrs) {
                    if (kv.first == "file") file = kv.second;
                    else if (kv.first == "label") label = kv.second;
                }
                Log(kLogInfo, "boot queued [" + name + "] file=" + file +
                                  " label=" + label);
                runner->Jump(file, label);
            } else {
                lua->DispatchTag(name, attrs);
            }
        }
        lua->SetJumpHandler([&runner](const std::string &file,
                                      const std::string &label) {
            // STATUS_RETURN: a return/stop just popped a frame — skip the
            // framework's trailing `tag{"jump" ui.asb return}` (dialog_return)
            // so it can't clobber the popfunc chain that resumes next.
            if (runner->Returning()) {
                Log(kLogInfo, "asb: skip jump while returning: " + label);
                return;
            }
            runner->Jump(file, label);
        });
        lua->SetCallHandler([&runner](const std::string &file,
                                      const std::string &label) {
            if (runner->Returning()) {
                Log(kLogInfo, "asb: skip call while returning: " + label);
                return;
            }
            runner->Call(file, label);
        });
        if (std::getenv("ARTC_CSV_TRACE"))
            lua->DoString(
                "local z=csv and csv.sysse and csv.sysse.sysvo or {}; "
                "print('PVZ|charlist='..tostring(z.charlist)); "
                "for nm,w in pairs(z) do "
                " if type(w)=='table' and nm~='charlist' and nm~='sysvowait' then "
                "  for n,x in pairs(w) do "
                "   local on=(conf and conf['svo_'..tostring(n)]==1); "
                "   print('PVZ|'..tostring(nm)..'['..tostring(n)..']='..type(x)..'#'..tostring(type(x)=='table' and #x or -1)..'|flag='..tostring(on)) "
                "  end end end",
                "csvprobe");
        lua->SetStopHandler([&runner, &lua](const std::string &tag) {
            // stop → 停机等待 eqtag 排水: defer the pop while a wait is queued
            // (dialog eqwait keeps the popfunc chain paused until answered).
            if (tag == "stop" && lua && lua->HasQueuedTag()) {
                Log(kLogInfo, "asb: stop deferred (queued wait)");
                return;
            }
            if (!runner->Return()) {
                runner->Halt();
                Log(kLogInfo, "asb: " + tag + " via lua tag");
            }
        });
        return true;
    };
    if (!boot()) return 1;

    // ---- frame loop (mirrors native_activity.cpp EngineThreadMain) ----
    bool waited = false;
    int engages = 0;
    // Drain the enqueueTag queue at a command boundary (after each script
    // command AND after input-dispatched calllua). The original engine
    // processes the queue right after every command; a queued "wait" (from
    // eqwait) engages the wait here, which stops further draining.
    auto drain = [&]() {
        while (lua && !lua->IsWaiting() && lua->HasQueuedTag()) {
            std::string name;
            std::vector<std::pair<std::string, std::string>> attrs;
            if (!lua->PopQueuedTag(&name, &attrs)) break;
            if (name == "jump" || name == "call") {
                std::string file, label;
                for (const auto &kv : attrs) {
                    if (kv.first == "file") file = kv.second;
                    else if (kv.first == "label") label = kv.second;
                }
                Log(kLogInfo, "queued [" + name + "] file=" + file +
                                  " label=" + label);
                if (runner->Returning()) {
                    Log(kLogInfo, "skip queued [" + name + "] while returning");
                } else {
                    runner->Jump(file, label);
                }
            } else {
                lua->DispatchTag(name, attrs);
            }
        }
    };
    for (int frame = 0; frame < frames; ++frame) {
        // 0) [reset] tag → engine reboot (re-run the boot chain)
        if (lua && lua->ConsumeResetRequest()) {
            Log(kLogInfo, "drive: reset requested; re-running boot chain");
            boot();
            waited = false;
        }
        if (lua && lua->ConsumeExitRequest()) {
            Log(kLogInfo, "drive: exit requested; stopping");
            break;
        }
        // 1) scheduled taps: mobile touch model = key 1 down/up + click.
        // A drag spec (x1,y1,x2,y2@frame) drives BeginDrag/DragMove/EndDrag.
        for (const DriveTap &tap : taps) {
            const int at = tap.frame < 0 ? 1 : tap.frame;  // negative = boot-time tap
            if (at != frame) continue;
            if (tap.drag) {
                lua->SetMousePoint(tap.x, tap.y);
                lua->SetTouchCount(1);
                lua->PushKeyDown(1);
                lua->BeginDrag(tap.x, tap.y);
                const int steps = 8;
                for (int s = 1; s <= steps; ++s) {
                    const float px = tap.x + (tap.x2 - tap.x) * s / steps;
                    const float py = tap.y + (tap.y2 - tap.y) * s / steps;
                    lua->SetMousePoint(px, py);
                    lua->DragMove(px, py);
                }
                lua->SetTouchCount(0);
                lua->EndDrag();
                lua->PushKeyUp(1);
                Log(kLogInfo, "drive: drag (" + std::to_string(tap.x) + "," +
                                  std::to_string(tap.y) + ") -> (" +
                                  std::to_string(tap.x2) + "," +
                                  std::to_string(tap.y2) + ") @ frame " +
                                  std::to_string(frame));
            } else {
                lua->SetMousePoint(tap.x, tap.y);
                lua->SetTouchCount(1);
                lua->PushKeyDown(1);
                lua->PushKeyUp(1);
                lua->SetTouchCount(0);
                lua->ClickAt(tap.x, tap.y);
                Log(kLogInfo, "drive: tap (" + std::to_string(tap.x) + "," +
                                  std::to_string(tap.y) + ") @ frame " +
                                  std::to_string(frame));
            }
            drain();   // click handler may enqueue (estag call etc.)
        }

        // 2) per-frame Lua work (framework vsync polls keys / drives loops)
        lua->RunEnterFrame();

        // 3) click-wait transitions (e:tag @ / p / rp / clickwait ↔ tap)
        const bool w = lua->IsWaiting();
        if (w && !waited) {
            ++engages;
            Log(kLogInfo, "drive: click-wait engaged @ frame " +
                              std::to_string(frame));
        } else if (!w && waited) {
            Log(kLogInfo, "drive: click-wait released @ frame " +
                              std::to_string(frame));
        }
        waited = w;

        // 4) step the native script runner (≤ 4 lines per frame)
        if (!w && runner && runner->Loaded() && !runner->Halted()) {
            for (int n = 0; n < 4 && runner->Loaded() && !runner->Halted(); ++n) {
                runner->ClearReturning();   // a return was resolved last line
                const AsbLine &ln = runner->Current();
                if (std::getenv("ARTC_STEP_TRACE"))
                    Log(kLogInfo, std::string("step pc=") +
                                      std::to_string(runner->CurrentIndex()) +
                                      " src# " + std::to_string(ln.lineno) +
                                      ": " +
                                      (ln.is_label ? "*" + ln.command
                                                   : "[" + ln.command + "]"));
                if (ln.is_label) {
                    runner->Advance();
                } else if (ln.command == "\x02LUA") {
                    for (const auto &kv : ln.attrs)
                        if (kv.first == "code") lua->DoString(kv.second, "asb:lua");
                    runner->Advance();
                } else if (ln.command == "calllua") {
                    std::string fn;
                    for (const auto &kv : ln.attrs)
                        if (kv.first == "function") fn = kv.second;
                    lua->CallGlobal(fn);
                    runner->Advance();
                } else if (ln.command == "jump") {
                    std::string lbl;
                    for (const auto &kv : ln.attrs)
                        if (kv.first == "label") lbl = kv.second;
                    runner->JumpTo(lbl);
                } else if (ln.command == "stop" || ln.command == "return") {
                    if (!runner->Return()) runner->Halt();
                } else {
                    lua->DispatchTag(ln.command, ln.attrs);
                    runner->Advance();
                }
                // command-boundary queue processing (estag call chains)
                drain();
                if (lua && lua->IsWaiting()) break;
            }
        }

        // 5) layer tracing: every layer's effective rect, once each
        compositor.DumpRects();

        // 6) clear per-frame input edges
        lua->EndFrame();
    }

    Log(kLogInfo, "drive: end frames=" + std::to_string(frames) +
                      " click-waits engaged=" + std::to_string(engages));
    return 0;
}

} // namespace artc