// lua_engine.h — Lua 5.1 embedding with the `e` bridge table.
//
// Behavior spec (from the decrypted iMel init.lua and engine analysis):
//   * `e:tag{ name, key=value, ... }` dispatches an engine tag; the `var` tag
//     stores system values (`system="os"|"screen_width"|"screen_height"`)
//   * `e:var(name)` reads a stored variable
//   * `e:isFileExists(path)` / `e:include(path)` resolve against the pack chain
//   * `e:debug(msg)` emits a debug line
//   * unknown `e.*` calls are logged as UNIMPLEMENTED (M0 discovery mechanism)
//   * globals DEBUG_MODE / DEBUG_LEVEL mirror the ini.DebugMode/ini.DebugLevel
//     settings and are settable from the script via the `debug` tag
#pragma once
#include "config/ini.h"
#include "pack/pack_manager.h"

#include "lua.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct lua_State;

namespace artc {

class Compositor;
class Audio;

class LuaEngine {
public:
    // `systemIni` supplies screen size / os defaults for the `var` tag.
    bool Init(PackManager *packs, const Ini &systemIni, const std::string &osName,
              int screenWidth, int screenHeight, Compositor *compositor = nullptr);
    // Load and execute a Lua source from the pack chain (e.g. "system/init.lua").
    bool RunPackScript(const std::string &path, std::string *errorOut);
    // Execute a Lua chunk (used by the iet [lua] blocks).
    bool DoString(const std::string &code, const std::string &chunk);
    // Call a global Lua function, passing the engine bridge table as arg 1
    // (first.iet: [calllua function="system_initlua"] receives the engine).
    bool CallGlobal(const std::string &fn);
    bool CallGlobalInternal(const std::string &fn, bool quiet);
    // Dispatch an engine tag through the e:tag bridge (iet [tag ...] lines).
    bool DispatchTag(const std::string &tag,
                     const std::vector<std::pair<std::string, std::string>> &attrs);

    // ---- input & frame hooks (M2.2) ----
    // The engine feeds normalized input (key ids per official key_id spec:
    // 1 = tap, 8 = back, 13 = enter, ...) and drives onEnterFrame each frame.
    void PushKeyDown(int key);          // press (sets down + down edge)
    void PushKeyUp(int key);            // release (clears down, sets up edge)
    void SetMousePoint(float x, float y);
    void SetTouchCount(int count);
    // Run the frame-bound Lua work: the registered onEnterFrame handler
    // (framework "vsync"). Call once per presented frame, then EndFrame.
    bool RunEnterFrame();
    void EndFrame();                    // clear per-frame edges
    // lifecycle audio mute (shell onPause/onResume hooks)
    void PauseAudio();
    void ResumeAudio();

    // The [jump file=… label=…] tag hands control to the native script runner.
    void SetJumpHandler(std::function<void(const std::string &file,
                                           const std::string &label)> cb) {
        jump_handler_ = std::move(cb);
    }
    // [call …] = [jump] with a return address (estag nesting); the runner
    // resumes from its saved frame when the called script [return]s.
    void SetCallHandler(std::function<void(const std::string &file,
                                           const std::string &label)> cb) {
        call_handler_ = std::move(cb);
    }

    // e:enqueueTag / eqtag — queued tags the engine runs at the next wait
    // point (the framework queues its startup jump this way).
    bool EnqueueTag(const std::string &name,
                    const std::vector<std::pair<std::string, std::string>> &attrs);
    // Pop one queued tag. Returns false when the queue is empty.
    bool PopQueuedTag(std::string *name,
                      std::vector<std::pair<std::string, std::string>> *attrs);
    bool HasQueuedTag() const { return !tag_queue_.empty(); }
    // Peek the name of the next queued tag ("" when empty) — lets the host
    // step loop pick up eqwait's pending "wait" at a command boundary.
    std::string QueuedTagName() const {
        return tag_queue_.empty() ? std::string() : tag_queue_.front().first;
    }
    void StoreLyevent(const std::string &id,
                      const std::map<std::string, std::string> &attrs) {
        std::string ty = "click";
        for (const auto &kv : attrs)
            if (kv.first == "type") ty = kv.second;
        lyevents_[id][ty] = {attrs.begin(), attrs.end()};
    }

    // A [stop]/[return] arriving as a tag (e.g. estag_call's final stop)
    // halts the native runner through this hook. The tag name ("stop" or
    // "return") is passed so the host can distinguish the real engine's
    // "stop → 停机等待 eqtag 排水" (pause, don't pop while a wait is queued)
    // from a [return] (always pops the frame).
    void SetStopHandler(std::function<void(const std::string &tag)> cb) {
        stop_handler_ = std::move(cb);
    }

    // Touch hit-test: topmost layer with a lyevent handler fires its
    // over+click Lua handlers (framework button model).
    void ClickAt(float x, float y);
    // Draggable-layer state machine (framework slider pins). The host frame
    // loop feeds: BeginDrag on key-1 down when the hit layer is draggable,
    // DragMove on every ACTION_MOVE pointer update, EndDrag on key-1 up.
    void BeginDrag(float x, float y);
    void DragMove(float x, float y);
    void EndDrag();
    // true while the pointer is down and a draggable layer was grabbed — the
    // following up must not fall through to ClickAt (button activation).
    bool DragActive() const { return !drag_id_.empty(); }

    // Locate the lyevent attr table for (layer, event type), walking up the
    // id hierarchy. With out==null only reports existence.
    bool FindLayerEvent(const std::string &id, const std::string &type,
                        std::vector<std::pair<std::string, std::string>> *out) const;

    // Click-wait gating: wait tags pause the native runner until a tap.
    void SetWaiting(bool w);
    void SetTimedWait(int ms);
    bool IsWaiting();

    // [reset] tag = engine reboot (language-select flow hands off this way).
    // The host loop polls this once per frame and re-runs the boot chain.
    void RequestReset() { reset_requested_ = true; }
    bool ConsumeResetRequest() {
        const bool r = reset_requested_;
        reset_requested_ = false;
        return r;
    }
    // [exit] tag = the framework's go_exit (title exit → dialog YES). The
    // host loop terminates the app once this is observed.
    void RequestExit() { exit_requested_ = true; }
    bool ConsumeExitRequest() {
        const bool r = exit_requested_;
        exit_requested_ = false;
        return r;
    }

    // System save (engine-side "save" tag, framework syssave→eqtag{"save"}):
    // persist the script-visible variables (set via e:tag{"var", name, data})
    // so a [reset] reboot can restore sys/conf/gscr. The host loop provides
    // the game directory (pack location) through SetSaveDir.
    void SetSaveDir(const std::string &dir) { save_dir_ = dir; }
    const std::string &SaveDir() const { return save_dir_; }
    // Persist / restore the script variable bank (fsave_pluto values) around
    // the [save] tag / a [reset] reboot.
    void SaveSystemData();
    void LoadSystemData();

    lua_State *state() const { return L_; }

private:
    static int l_tag(lua_State *L);
    static int l_var(lua_State *L);
    static int l_isFileExists(lua_State *L);
    static int l_include(lua_State *L);
    static int l_file(lua_State *L);
    static int l_setMagicPath(lua_State *L);
    static int l_isDown(lua_State *L);
    static int l_isDownEdge(lua_State *L);
    static int l_isUpEdge(lua_State *L);
    static int l_getMousePoint(lua_State *L);
    static int l_getTouchCount(lua_State *L);
    static int l_setEventHandler(lua_State *L);
    static int l_overrideKey(lua_State *L);
    static int l_enqueueTag(lua_State *L);
    static int l_random(lua_State *L);
    static int l_getScriptStack(lua_State *L);
    static int l_getScriptWaitReason(lua_State *L);
    static int l_lyevent(lua_State *L);
    bool PushGlobalFn(const std::string &fn, bool quiet);
    void CallEvent(const std::string &fn,
                   const std::vector<std::pair<std::string, std::string>> &param,
                   bool quiet);
    void FireOnPush(int key);   // press dispatch → registered setonpush handler
    static int l_debug(lua_State *L);
    static int l_now(lua_State *L);
    static int l_noop(lua_State *L);
    static int l_index(lua_State *L);         // __index → logging stub closure
    static int l_stub(lua_State *L);          // fallback for unimplemented e.*
    static int l_allkeyoff(lua_State *L);

    static LuaEngine *Self(lua_State *L);

    // Magic path registry (e:setMagicPath{"bg", "background"}): a pack path
    // starting with ":word/rest" resolves to "<registered>/rest" (official
    // spec: magic word runs from ':' to the next slash, only at path start).
    std::string ResolvePackPath(const std::string &path) const;

    PackManager *packs_ = nullptr;
    Compositor *compositor_ = nullptr;
    lua_State *L_ = nullptr;
    std::map<std::string, std::string> vars_;    // script-visible variables
    std::map<std::string, std::string> sysvals_; // engine system values (os, screen_width, ...)
    std::map<std::string, std::string> magic_paths_;
    std::map<std::string, std::string> event_handlers_; // onEnterFrame -> Lua fn name
    int enterframe_failures_ = 0;                       // error-log rate limiting
    std::map<std::string,
                 std::map<std::string,
                          std::vector<std::pair<std::string, std::string>>>>
        lyevents_;   // layer id → event type (click/dragin/drag/dragout) → attrs
    // setonpush/delonpush registry (framework key→handler): the CLICK key
    // (tap) drives button activation/dialog routing via setonpush_calllua.
    std::map<int, std::vector<std::pair<std::string, std::string>>> onpush_;
    // active draggable drag (slider pin). drag_id_ is the layer being moved;
    // drag_origin_ = pointer stage pos at press; drag_off_ = layer's stored
    // left/top offset at press (dragarea-relative).
    std::string drag_id_;
    float drag_origin_x_ = 0, drag_origin_y_ = 0;
    float drag_off_x_ = 0, drag_off_y_ = 0;
    bool waiting_ = false;                              // click-wait gating
    bool timed_wait_ = false;
    // A [trans] tag began a transition; the engine has no transition tween,
    // so the transition's own completion wait (wt / trans_flag eqwait) must
    // auto-complete instead of blocking on user input.
    bool transition_wait_ = false;
    // audio backend (splay/seplay/voplay) — raw ptr, owned by this engine
    Audio *audio_ = nullptr;
    // message pipeline: chgmsg-selected layer + accumulated print text
    std::string msg_layer_;
    std::string msg_text_;
    // `font` tag state: face load-once + per-layer text-area rects.
    // Official semantics: a `font` tag restyles the layer selected by the
    // LAST chgmsg (font_of_), so each message slot keeps its own rect.
    bool font_loaded_ = false;
    std::map<std::string, std::map<std::string, std::string>> font_of_;
    std::map<std::string, std::string> font_main_;   // widest visible area
    std::map<std::string, std::string> font_name_;   // small visible area
    std::chrono::steady_clock::time_point wait_until_;
    std::set<std::string> seen_tags_;                   // first-occurrence tag trace
    int msg_layer_height_ = 50;                     // font tag height → get_message_layer_height
    std::function<void(const std::string &, const std::string &)> jump_handler_;
    std::function<void(const std::string &, const std::string &)> call_handler_;
    std::function<void(const std::string &)> stop_handler_;
    std::deque<std::pair<std::string,
                         std::vector<std::pair<std::string, std::string>>>> tag_queue_;
    bool reset_requested_ = false;
    bool exit_requested_ = false;
    std::string save_dir_;   // game directory for system.dat / saves
    bool key_down_[256] = {};
    bool key_down_edge_[256] = {};
    bool key_up_edge_[256] = {};
    float mouse_x_ = 0, mouse_y_ = 0;
    int touch_count_ = 0;
    int debug_mode_ = 0;
    int debug_level_ = 0;
    std::chrono::steady_clock::time_point init_time_;

public:
    ~LuaEngine(); // closes the lua_State
};

} // namespace artc
