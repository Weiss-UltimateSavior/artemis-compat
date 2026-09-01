#include "script/lua_engine.h"
#include "render/compositor.h"
#include "audio/audio.h"
#include "pack/pack_manager.h"
#include "script/pluto_lua.h"
#include "log/logger.h"

#include <cstring>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <set>

extern "C" {
#include "lauxlib.h"   // luaL_traceback (debug support); vendored with lua.hpp
}

namespace artc {

namespace {
const char kBridgeTable[] = "e";
char kEngineKey;
char kPackKey;

// Engine-level blocking waits. "@" is the explicit click-wait tag (the asb
// *click2 native line and framework exclick/keycode_click/txkey_click emit it);
// "clickwait" is a long-form alias. "p" is kept for legacy scripts that use
// the pre-@ spelling. "rp" is deliberately NOT here: the official tag docs
// define it as a scenario *page split* (message layer), and the framework
// dispatches it through e:tag during normal page display — treating it as a
// wait deadlocks the runner at every paragraph.
bool IsClickWaitTag(const std::string &tag) {
    return tag == "@" || tag == "p" || tag == "clickwait";
}
} // namespace

LuaEngine *LuaEngine::Self(lua_State *L) {
    lua_pushlightuserdata(L, reinterpret_cast<void *>(&kEngineKey));
    lua_gettable(L, LUA_REGISTRYINDEX);
    LuaEngine *self = static_cast<LuaEngine *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    return self;
}

LuaEngine::~LuaEngine() {
    if (audio_) { delete audio_; audio_ = nullptr; }
    if (L_) lua_close(L_);
}
void LuaEngine::PauseAudio() { if (audio_) audio_->PauseAll(); }
void LuaEngine::ResumeAudio() { if (audio_) audio_->ResumeAll(); }

// ---- input & frame hooks ----

void LuaEngine::PushKeyDown(int key) {
    if (key < 0 || key >= 256) return;
    if (!key_down_[key]) key_down_edge_[key] = true;
    key_down_[key] = true;
}

void LuaEngine::PushKeyUp(int key) {
    if (key < 0 || key >= 256) return;
    if (key_down_[key]) key_up_edge_[key] = true;
    key_down_[key] = false;
}

void LuaEngine::SetMousePoint(float x, float y) { mouse_x_ = x; mouse_y_ = y; }
void LuaEngine::SetTouchCount(int count) { touch_count_ = count; }

void LuaEngine::EndFrame() {
    std::memset(key_down_edge_, 0, sizeof(key_down_edge_));
    std::memset(key_up_edge_, 0, sizeof(key_up_edge_));
}

bool LuaEngine::RunEnterFrame() {
    const auto it = event_handlers_.find("onEnterFrame");
    if (it == event_handlers_.end() || it->second.empty()) return false;
    // rate-limit error spam: a broken vsync would otherwise log at frame rate
    const bool quiet = enterframe_failures_ > 0 && enterframe_failures_ % 600 != 0;
    const bool ok = CallGlobalInternal(it->second, quiet);
    if (ok) enterframe_failures_ = 0;
    else ++enterframe_failures_;
    return ok;
}

bool LuaEngine::Init(PackManager *packs, const Ini &systemIni,
                     const std::string &osName, int screenWidth, int screenHeight,
                     Compositor *compositor) {
    packs_ = packs;
    compositor_ = compositor;
    audio_ = new Audio();
    audio_->Init(packs);
    L_ = luaL_newstate();
    if (!L_) return false;
    init_time_ = std::chrono::steady_clock::now();
    luaL_openlibs(L_);
    // register the pluto serializer (save/load data format)
    if (luaL_dostring(L_, PLUTO_LUA_SRC) != 0) {
        Log(kLogError, std::string("pluto registration failed: ") + lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }

    // expose engine instance + packs to the C closures via the registry
    lua_pushlightuserdata(L_, reinterpret_cast<void *>(&kEngineKey));
    lua_pushlightuserdata(L_, this);
    lua_settable(L_, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L_, reinterpret_cast<void *>(&kPackKey));
    lua_pushlightuserdata(L_, packs);
    lua_settable(L_, LUA_REGISTRYINDEX);

    // engine settings used by the `var` tag
    sysvals_["os"] = osName;
    sysvals_["screen_width"] = std::to_string(screenWidth);
    sysvals_["screen_height"] = std::to_string(screenHeight);
    sysvals_["engineversion"] = "3.00";
    sysvals_["savedataversion"] = "100";
    sysvals_["llp64"] = "1"; // arm64/x64 builds are LP64 (boot.lua platform check)
    sysvals_["status.automode"] = "0";
    sysvals_["status.commandskip"] = "0";
    sysvals_["status.controlskip"] = "0";
    sysvals_["status.alreadyread"] = "0";
    sysvals_["status.avoid"] = "0";
    if (osName == "windows") {
        sysvals_["windowsversion"] = "6.2";
        sysvals_["windowsfeaturelevel"] = "10.0";
    }
    vars_["t.os"] = osName;
    vars_["t.w"] = std::to_string(screenWidth);
    vars_["t.h"] = std::to_string(screenHeight);
    // default layer rect (t.ly.*) — tablet UI computes centers from these
    vars_["t.ly.left"] = "0";
    vars_["t.ly.top"] = "0";
    vars_["t.ly.width"] = std::to_string(screenWidth);
    vars_["t.ly.height"] = std::to_string(screenHeight);

    // ---- the `e` bridge table ----
    lua_newtable(L_);
    static const struct {
        const char *name;
        lua_CFunction fn;
    } methods[] = {
        {"tag", l_tag}, {"var", l_var}, {"isFileExists", l_isFileExists},
        {"include", l_include}, {"debug", l_debug}, {"now", l_now},
        {"file", l_file},
        {"setTagFilter", l_noop},
        {"setMagicPath", l_setMagicPath},
        {"setUseMultiTouch", l_noop},
        {"setUseTouchHold", l_noop},
        {"random", l_random},
        {"setEventFilter", l_noop},
        {"setEventHandler", l_setEventHandler},
        {"enqueueTag", l_enqueueTag},
        {"bindSurfaceAsync", l_noop},
        {"isDown", l_isDown},
        {"isDownEdge", l_isDownEdge},
        {"isUpEdge", l_isUpEdge},
        {"getMousePoint", l_getMousePoint},
        {"getTouchCount", l_getTouchCount},
        {"overrideKey", l_overrideKey},
        {"lyevent", l_lyevent},
        {"getScriptStack", l_getScriptStack},
        {"getScriptWaitReason", l_getScriptWaitReason},
        {"bindSurface", l_noop},
        {"clearSurfaceLoadQueue", l_noop},
    };
    for (const auto &m : methods) {
        lua_pushcfunction(L_, m.fn);
        lua_setfield(L_, -2, m.name);
    }
    // unknown e.* members resolve to a logging stub (closure carries the name)
    lua_newtable(L_); // metatable
    lua_pushcfunction(L_, l_index);
    lua_setfield(L_, -2, "__index");
    lua_setmetatable(L_, -2);
    lua_setglobal(L_, kBridgeTable);

    // globals used by the framework
    lua_pushcfunction(L_, l_allkeyoff);
    lua_setglobal(L_, "allkeyoff");
    lua_pushinteger(L_, debug_mode_);
    lua_setglobal(L_, "DEBUG_MODE");
    lua_pushinteger(L_, debug_level_);
    lua_setglobal(L_, "DEBUG_LEVEL");

    // restore persisted variables (fsave_pluto bank) before the boot scripts
    // run — system_dataloading's fload_pluto reads them through e:var.
    LoadSystemData();
    // Config-table safety net: the framework builds `conf` during boot, but
    // boot-chain code (estag media/sysvo) may touch it earlier — sysvo's
    // generic path reads conf["svo_*"] for the enabled-character list.
    lua_getglobal(L_, "conf");
    if (lua_isnil(L_, -1)) {
        lua_newtable(L_);
        lua_setglobal(L_, "conf");
    }
    lua_pop(L_, 1);
    // sysvo personality hook: the media framework calls _G["user_func_sysvo"]
    // (ex.user_sysvo) when present. Until game voices are implemented, install
    // a no-op so the generic sysvo path (which reads the sysvo csv tables)
    // isn't the fall-through — voice playback stays silent and non-fatal.
    lua_getglobal(L_, "user_func_sysvo");
    if (lua_isnil(L_, -1)) {
        lua_pushcfunction(L_, [](lua_State *) { return 0; });
        lua_setglobal(L_, "user_func_sysvo");
    }
    lua_pop(L_, 1);
    // movie_play guard: the asb *movie_play chain (movie_init → calllua
    // movie_play) runs on the "continue from save" path too, but our save
    // bank doesn't persist `scr.movie` — so scr.movie is nil there and
    // movie_play crashes at `local p = scr.movie; p.file`. Until real movie
    // playback lands, skip the chain when there's nothing to play; the
    // framework's [wt] after it still yields to the player.
    DoString(
        "if not _artc_mp_guard then\n"
        " local _real = _G.movie_play\n"
        " _artc_mp_guard = true\n"
        " _G.movie_play = function()\n"
        "  if not scr or not scr.movie then return end\n"
        "  return _real()\n"
        " end\n"
        "end", "movie-shim");
    return true;
}

// e:tag{ "tagname", key=value, ... } — M0: log + implement `var` and `debug`.
int LuaEngine::l_tag(lua_State *L) {
    LuaEngine *self = Self(L);
    if (!lua_istable(L, 2)) return 0;

    // element 1 of the array part is the tag name
    lua_rawgeti(L, 2, 1);
    const char *tag = lua_tostring(L, -1);
    std::string tagname = tag ? tag : "";
    lua_pop(L, 1);

    if (tagname == "var") {
        // {"var", name="t.os", system="os"} → store the engine system value
        lua_getfield(L, 2, "name");
        const char *name = lua_tostring(L, -1);
        // {"var", name=…, data=pluto串} — fsave_pluto bank. The [save] tag
        // persists these; on reboot LoadSystemData re-injects them so the
        // framework's fload_pluto→e:var can restore sys/conf/gscr.
        lua_getfield(L, 2, "data");
        const char *data = lua_tostring(L, -1);
        if (name && data) {
            self->vars_[name] = data;
            lua_pop(L, 2);
            return 0;
        }
        lua_pop(L, 1);   // nil data
        lua_getfield(L, 2, "system");
        const char *sys = lua_tostring(L, -1);
        if (name && sys) {
            if (std::string(sys) == "get_message_layer_height") {
                // Framework query (msg/ui.lua uihelp_over): the message
                // layer's content height after font layout — used to center
                // UI help text vertically. We approximate with the height of
                // the most recent `font` tag.
                self->vars_[name] = std::to_string(self->msg_layer_height_);
            } else if (std::string(sys) == "get_layer_info") {
                // slider_dragX reads the pinned layer's stored left to derive
                // the drag percentage — return the raw stored offsets.
                lua_getfield(L, 2, "id");
                const char *lid = lua_tostring(L, -1);
                lua_pop(L, 1);
                if (lid && self->compositor_) {
                    const auto info = self->compositor_->GetLayerInfo(lid);
                    if (info.found) {
                        const std::string nm(name);
                        self->vars_[nm] = std::to_string((int)info.left);
                        // width/height are queried too (percent base)
                        self->vars_[nm + ".width"] = std::to_string((int)info.width);
                        self->vars_[nm + ".height"] = std::to_string((int)info.height);
                    }
                }
            } else {
                auto it = self->sysvals_.find(sys);
                self->vars_[name] = it == self->sysvals_.end() ? sys
                                                               : it->second;
            }
        }
        lua_pop(L, 2);
        return 0;
    }
    if (tagname == "font") {
        // Message-layer font parameters; the height feeds the engine query
        // get_message_layer_height (see above).
        lua_getfield(L, 2, "height");
        const int h = static_cast<int>(lua_tointeger(L, -1));
        if (h > 0) self->msg_layer_height_ = h;
        lua_pop(L, 1);
    }
    if (tagname == "debug") {
        lua_getfield(L, 2, "mode");
        lua_getfield(L, 2, "level");
        self->debug_mode_ = static_cast<int>(lua_tointeger(L, -2));
        self->debug_level_ = static_cast<int>(lua_tointeger(L, -1));
        lua_pushinteger(L, self->debug_mode_);
        lua_setglobal(L, "DEBUG_MODE");
        lua_pushinteger(L, self->debug_level_);
        lua_setglobal(L, "DEBUG_LEVEL");
        lua_pop(L, 2);
        Log(kLogInfo, "debug tag: mode=" + std::to_string(self->debug_mode_) +
                          " level=" + std::to_string(self->debug_level_));
        return 0;
    }
    Log(kLogDebug, "tag: " + tagname);
    // route graphics tags to the compositor
    LuaEngine *inst = Self(L);
    if (inst) {
        // first-occurrence tag trace: reveals what the scripts ask the engine
        // for without flooding logcat at frame rate
        std::string summary = tagname;
        {
            lua_pushnil(L);
            bool first = true;
            while (lua_next(L, 2) != 0) {
                if (lua_type(L, -2) == LUA_TSTRING) {
                    const char *k = lua_tostring(L, -2);
                    const char *v = lua_tostring(L, -1);
                    if (k && v) {
                        summary += first ? " {" : ", ";
                        first = false;
                        summary += std::string(k) + "=" + v;
                    }
                }
                lua_pop(L, 1);
            }
            if (!first) summary += "}";
        }
        if (inst->seen_tags_.insert(summary).second) {
            Log(kLogInfo, "tag[trace]: " + summary);
        }
    }
    // Build the string-attribute map once. Only string keys are tag
    // attributes (array key 1 = tag name); lua_tostring would convert a
    // NUMBER key in place and corrupt lua_next iteration ("invalid key to
    // 'next'"), so guard by type. Used by the wait gate and the compositor.
    std::map<std::string, std::string> m;
    if (inst) {
        lua_pushnil(L);
        while (lua_next(L, 2) != 0) {
            if (lua_type(L, -2) == LUA_TSTRING) {
                const char *k = lua_tostring(L, -2);
                const char *v = lua_tostring(L, -1); // numbers: value slot, safe
                if (k && v) m[k] = v;
            }
            lua_pop(L, 1);
        }
    }
    if (inst && IsClickWaitTag(tagname)) {
        inst->SetWaiting(true);   // pause the native runner until the next tap
        return 0;
    }
    if (tagname == "wait" && inst) {
        // Official wait semantics (spec/tag/script/wait.md): suspend the
        // script until a user input (input=1/2) and/or the time (ms) elapses.
        // `scenario` gates on the message/transition tween instead — our
        // engine has no tween, so those complete immediately (the story
        // page click is gated by the *click2 `@` tag, not this wait).
        // A [trans] just began: its wt/trans_flag completion wait must
        // auto-complete too (no transition animation in this engine).
        const std::string sc = m["scenario"];
        if (!sc.empty() || inst->transition_wait_) {
            inst->transition_wait_ = false;
            return 0;   // scenario / transition tween → auto-complete
        }
        int time = 0;
        const auto it = m.find("time");
        if (it != m.end()) time = std::atoi(it->second.c_str());
        const auto i0 = m.find("0");
        if (time == 0 && i0 != m.end()) time = std::atoi(i0->second.c_str());
        const std::string in = m["input"];
        if (in == "1" || in == "2") {
            if (time > 0) inst->SetTimedWait(time);   // either input or deadline
            else inst->SetWaiting(true);              // pure click wait
        } else if (time > 0) {
            inst->SetTimedWait(time);
        } else {
            // input=0, time=0: eqwait normalized by flg.ui (dialog eqwait /
            // trans_flag) — blocks the runner, released by any tap.
            inst->SetWaiting(true);
        }
        return 0;
    }
    if ((tagname == "stop" || tagname == "return") && inst && inst->stop_handler_) {
        inst->stop_handler_(tagname);
        return 0;
    }
    // estag chains are driven by `call system/script.asb label=estagNN` tags
    // (estag("...") → e:tag{"call", ...}); without this the whole estag
    // machinery (language-select wait, title/transition chains, syssave/reset)
    // silently no-ops. Nesting chains use the runner call-stack (estag_call
    // chains start inside each other — e.g. uitrans starts its own mid-way).
    if (tagname == "jump" && inst && inst->jump_handler_ &&
        !m["file"].empty()) {
        inst->jump_handler_(inst->ResolvePackPath(m["file"]), m["label"]);
        return 0;
    }
    if (tagname == "call" && inst && inst->call_handler_ && !m["file"].empty()) {
        inst->call_handler_(inst->ResolvePackPath(m["file"]), m["label"]);
        return 0;
    }
    // reset = engine reboot (official spec: 重启引擎). The language-selection
    // flow ends with estag{uitrans, syssave, reset}: after the player picks a
    // language the engine restarts so the boot chain re-runs with the new
    // config. syssave() only *queues* the [save] tag (eqtag) — a reboot would
    // drop that queue, so flush the variable bank to disk first.
    if (tagname == "reset" && inst) {
        inst->SaveSystemData();
        inst->RequestReset();
        return 0;
    }
    // [exit] — title exit dialog YES → sv.go_exit → ui.asb *go_exit → engine
    // terminates the process (host loop + Android activity finish).
    if (tagname == "exit" && inst) {
        inst->RequestExit();
        return 0;
    }
    // [save] — the framework's syssave() ends with eqtag{"save"}: persist
    // the script variable bank (fsave_pluto values) to disk so a [reset]
    // reboot can restore them (language config, system data, …).
    if (tagname == "save" && inst) {
        inst->SaveSystemData();
        return 0;
    }
    if (inst && inst->compositor_) {
        // calllua (framework call_lua()): button exec / p4 callbacks arrive
        // as an e:tag {"calllua", function="fn", ...} — e.g. the language
        // buttons' exec="langsel_click". The framework convention is
        // fn(e, tag_attrs): the attrs table (key/name/btn/...) is param 2 —
        // langsel_click reads p.btn. (The asb runner's native calllua lines
        // take the function-only path and don't pass through here.)
        if (tagname == "calllua" && m.count("function") &&
            !m["function"].empty()) {
            if (m["function"] == "fn.pop")
                inst->DoString("local fs=flg and flg.funcstack\n"
                         "local c=fn and fn.name and fs and fs[fn.name]\n"
                         "e:debug('POP stack='..tostring(fn and fn.name)"
                         "..' n='..tostring(c and #c)"
                         "..' param='..tostring(fn and fn.param))", "pop");
            std::vector<std::pair<std::string, std::string>> params(m.begin(),
                                                                    m.end());
            inst->CallEvent(m["function"], params, false);
            return 0;
        }
        if (tagname == "lyevent" && m.count("id")) {
            // The framework emits click/rollover/rollout lyevent tags for the
            // SAME layer id; only the click registration carries the action
            // handler (function=btn_clickex) plus over/click attrs. Keep it
            // and ignore the siblings so `function` isn't overwritten.
            if (m["type"] != "click" && inst->lyevents_.count(m["id"]))
                return 0;
            inst->StoreLyevent(m["id"], m);
            return 0;
        }
        if (tagname == "lyc" && m.count("id") && m.count("file")) {
            if (inst->packs_) {
                inst->compositor_->SetPackManager(inst->packs_);
                inst->compositor_->LoadImage(m["id"],
                                             inst->ResolvePackPath(m["file"]));
            }
            return 0;
        }
        if (tagname == "lyprop" && m.count("id")) {
            inst->compositor_->SetProps(m["id"], m);
            return 0;
        }
        // lytween — the framework hides title buttons (alpha=0) then reveals
        // them with 0.4s alpha/left tweens (title_animeset/title_anime). Our
        // compositor has no animation engine, so apply the tween end value
        // immediately (instant-reveal approximation; animation comes later).
        if (tagname == "lytween" && m.count("id") && m.count("param") &&
            m.count("to")) {
            inst->compositor_->SetProps(m["id"],
                                        {{m["param"], m["to"]}});
            return 0;
        }
        if ((tagname == "setonpush" || tagname == "delonpush") && inst) {
        // Framework key-callback registry: setonpush{key=N, function, adv, ui,
        // btn} — when key N is pressed the engine calls setonpush_calllua,
        // which routes button activation / dialog / UI key handling. Without
        // this, tap-driven UI (dialog yes/no, key nav) stays dead.
        const auto &ks = m.find("key");
        const int key = ks == m.end() ? 0 : std::atoi(ks->second.c_str());
        if (key > 0) {
            if (tagname == "setonpush") {
                inst->onpush_[key] = {m.begin(), m.end()};
            } else {
                inst->onpush_.erase(key);
            }
        }
        return 0;
    }
    if (tagname == "lydel" && m.count("id")) {
            inst->compositor_->DeleteLayer(m["id"]);
            // Drop click handlers of the deleted subtree (like the title
            // button group "500"): otherwise stale buttons registered via
            // lyevent keep receiving hit-test hits inside the story.
            const std::string lid = m["id"];
            const std::string pre = lid + ".";
            for (auto it = inst->lyevents_.begin(); it != inst->lyevents_.end();) {
                if (it->first == lid ||
                    it->first.compare(0, pre.size(), pre) == 0)
                    it = inst->lyevents_.erase(it);
                else
                    ++it;
            }
            return 0;
        }
        if (tagname == "flip") {
            inst->compositor_->Draw();
            return 0;
        }
        // [trans] — transition effect. No transition animation in this
        // engine; mark that the transition's own completion wait (wt /
        // trans_flag eqwait) must auto-complete.
        if (tagname == "trans") {
            inst->transition_wait_ = true;
            return 0;
        }
        // ---- message pipeline (chgmsg / print / rt) ----
        // Framework (msg/message.lua): chgmsg_adv selects the message text
        // layer via e:tag{"chgmsg", id=...}; mw_textloop then emits
        // print{data} rows (speaker name / line text) into that layer, rt
        // ends a line, /chgmsg closes the selection. Official print
        // rasterizes `data` into the selected layer (engine-side text).
        // Single-line approximation: the whole page accumulates into one
        // raster; rt and /chgmsg only reset the accumulator for now.
        if (tagname == "font" && inst) {
            // Load the face once. The tag restyles the CURRENT chgmsg layer;
            // fonts with show=none belong to hidden/off-screen slots (e.g.
            // top=-5 measure slots) and must never become the visible layout.
            if (!inst->font_loaded_) {
                auto face = m.find("face");
                if (face != m.end()) {
                    inst->compositor_->SetPackManager(inst->packs_);
                    if (inst->compositor_->LoadFont(
                            inst->ResolvePackPath(face->second)))
                        inst->font_loaded_ = true;
                }
            }
            if (!inst->msg_layer_.empty())
                inst->font_of_[inst->msg_layer_] = {m.begin(), m.end()};
            auto hidden = m.find("show");
            if (hidden != m.end() && hidden->second == "none") return 0;
            auto &slot = [&]() -> std::map<std::string, std::string> & {
                auto w = m.find("width");
                return (w != m.end() && std::atof(w->second.c_str()) >= 700)
                           ? inst->font_main_
                           : inst->font_name_;
            }();
            slot = {m.begin(), m.end()};
            return 0;
        }
        if (tagname == "chgmsg" && inst) {
            auto it = m.find("id");
            inst->msg_layer_ = it == m.end() ? std::string() : it->second;
            inst->msg_text_.clear();
            return 0;
        }
        if (tagname == "/chgmsg" && inst) {
            inst->msg_layer_.clear();
            inst->msg_text_.clear();
            return 0;
        }
        if (tagname == "print" && inst && !inst->msg_layer_.empty()) {
            inst->msg_text_ += m["data"];
            // font resolution: the rect registered for THIS layer via
            // chgmsg+font pairing wins; otherwise the last visible-area font.
            static const std::map<std::string, std::string> kEmpty;
            auto fo = inst->font_of_.find(inst->msg_layer_);
            const std::map<std::string, std::string> &fr =
                fo != inst->font_of_.end()
                    ? fo->second
                    : (inst->msg_layer_.find("name") != std::string::npos
                           ? inst->font_name_
                           : inst->font_main_);
            const float size =
                fr.count("size") ? std::atof(fr.at("size").c_str()) : 40.f;
            uint32_t color = 0xFFFFFF;
            if (fr.count("color"))
                color = static_cast<uint32_t>(
                    strtoul(fr.at("color").c_str(), nullptr, 16));
            float wrap = 0;
            std::string wrap_src = "none";
            {
                auto we = fr.find("width");
                if (we != fr.end()) { wrap = std::atof(we->second.c_str()); wrap_src = "rect"; }
                if (wrap <= 10 || wrap > 2000) {
                    // The per-layer rect may be missing for the message body
                    // layer; fall back to a sane message-window line width so
                    // long lines wrap inside the window instead of spilling to
                    // the right edge. ~850 stage units fits a 1280 stage with a
                    // 284 left margin and leaves a right margin.
                    wrap = 850.0f; wrap_src = "default";
                }
                Log(kLogInfo, "print: layer=" + inst->msg_layer_ + " wrap=" +
                                  std::to_string((int)wrap) + "(" + wrap_src + ") size=" +
                                  std::to_string((int)size));
            }
            if (inst->compositor_->SetText(
                    inst->msg_layer_, inst->msg_text_, size, color, wrap)) {
                std::map<std::string, std::string> pos;
                if (fr.count("left")) pos["left"] = fr.at("left");
                if (fr.count("top")) pos["top"] = fr.at("top");
                if (!pos.empty()) inst->compositor_->SetProps(inst->msg_layer_, pos);
            }
            return 0;
        }
        if (tagname == "rt" && inst) {
            return 0;   // line break folded into the single-line raster
        }
        // ---- audio: splay(loop BGM) / seplay(SE) / voplay(voice) / sstop ----
        // Official tags: splay{ id=, file=, loop=, volume=, gain= }; seeking
        // silence with no unit is fine — volume/gain 0-1000 map to our player.
        if (inst && (tagname == "splay" || tagname == "seplay" ||
                     tagname == "voplay" || tagname == "vbplay" ||
                     tagname == "bplay" || tagname == "s2play")) {
            if (inst->audio_) {
                auto fit = m.find("file");
                if (fit != m.end()) {
                    const std::string key = m.count("id") && !m.at("id").empty()
                                                ? m.at("id") : fit->second;
                    const bool loop = m.count("loop") && m.at("loop") == "1";
                    int vol = 1000;
                    auto vit = m.find("volume");
                    if (vit != m.end()) vol = std::atoi(vit->second.c_str());
                    else { auto git = m.find("gain"); if (git != m.end()) vol = std::atoi(git->second.c_str()); }
                    inst->audio_->Play(key, inst->ResolvePackPath(fit->second), loop, vol);
                }
            }
            return 0;
        }
        if (inst && tagname == "sstop" && inst->audio_) {
            auto it = m.find("id");
            if (it != m.end()) inst->audio_->Stop(m.at("id"));
            else inst->audio_->StopAll();
            return 0;
        }
    }
    return 0;
}

// e:var(name) — script vars first, then engine system values (s.*), else ""
int LuaEngine::l_var(lua_State *L) {
    LuaEngine *self = Self(L);
    const char *name = luaL_checkstring(L, 2);
    auto it = self->vars_.find(name);
    if (it == self->vars_.end()) {
        // system values are stored bare ("engineversion"); the script queries
        // them with the "s." prefix ("s.engineversion")
        std::string key(name);
        if (key.rfind("s.", 0) == 0) key.erase(0, 2);
        auto sit = self->sysvals_.find(key);
        Log(kLogDebug, std::string("e:var('") + name + "') -> sys " +
                           (sit == self->sysvals_.end() ? "(miss)" : sit->second));
        lua_pushstring(L, sit == self->sysvals_.end() ? "" : sit->second.c_str());
    } else {
        lua_pushstring(L, it->second.c_str());
    }
    return 1;
}

// e:isFileExists(path)
// e:file(path) — file content as a string from the pack chain (parseIni feeds
// on this; the adv framework's config tables are built from it).
int LuaEngine::l_file(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushlightuserdata(L, reinterpret_cast<void *>(&kPackKey));
    lua_gettable(L, LUA_REGISTRYINDEX);
    PackManager *packs = static_cast<PackManager *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const char *path = luaL_checkstring(L, 2);
    const std::string resolved = self->ResolvePackPath(path);

    std::vector<uint8_t> bytes;
    if (!packs || !packs->Read(resolved, bytes)) {
        Log(kLogWarn, "file: not found in packs: " + resolved);
        lua_pushnil(L);
        return 1;
    }
    lua_pushlstring(L, reinterpret_cast<const char *>(bytes.data()), bytes.size());
    return 1;
}

int LuaEngine::l_isFileExists(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushlightuserdata(L, reinterpret_cast<void *>(&kPackKey));
    lua_gettable(L, LUA_REGISTRYINDEX);
    PackManager *packs = static_cast<PackManager *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const char *path = luaL_checkstring(L, 2);
    lua_pushboolean(L, packs && packs->Exists(self->ResolvePackPath(path)) ? 1 : 0);
    return 1;
}

// e:setMagicPath{word, path} — register the ":word" path alias (official spec:
// empty path unregisters; the magic word may only appear at the path start).
int LuaEngine::l_setMagicPath(lua_State *L) {
    LuaEngine *self = Self(L);
    const char *word = nullptr;
    const char *target = nullptr;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        word = lua_tostring(L, -1);
        lua_rawgeti(L, 2, 2);
        target = lua_tostring(L, -1);
        lua_pop(L, 2);
    } else if (lua_isstring(L, 2) && lua_isstring(L, 3)) {
        // tolerate the (word, path) two-argument form
        word = lua_tostring(L, 2);
        target = lua_tostring(L, 3);
    }
    if (!word) {
        Log(kLogWarn, "setMagicPath: missing magic word");
        return 0;
    }
    if (target && target[0] != '\0') {
        self->magic_paths_[word] = target;
        Log(kLogDebug, std::string("setMagicPath: ") + word + " -> " + target);
    } else {
        self->magic_paths_.erase(word);
        Log(kLogDebug, std::string("setMagicPath: unregistered ") + word);
    }
    return 0;
}

// ":word/rest" → "<registered for word>/rest" (official spec: word runs from
// ':' to the next slash/backslash and is only valid at the path start).
std::string LuaEngine::ResolvePackPath(const std::string &path) const {
    if (path.empty() || path[0] != ':') return path;
    const std::string body = path.substr(1);
    const size_t slash = body.find_first_of("/\\");
    const std::string word = slash == std::string::npos ? body : body.substr(0, slash);
    const std::string rest = slash == std::string::npos ? "" : body.substr(slash + 1);
    const auto it = magic_paths_.find(word);
    if (it == magic_paths_.end()) {
        Log(kLogWarn, "magic path not registered: " + path);
        return path;
    }
    if (rest.empty()) return it->second;
    return it->second + "/" + rest;
}

// ---- input polling bridge (official isPush/isDown/getMousePoint family) ----

int LuaEngine::l_isDown(lua_State *L) {
    LuaEngine *self = Self(L);
    const int key = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, self && key >= 0 && key < 256 && self->key_down_[key] ? 1 : 0);
    return 1;
}

int LuaEngine::l_isDownEdge(lua_State *L) {
    LuaEngine *self = Self(L);
    const int key = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, self && key >= 0 && key < 256 && self->key_down_edge_[key] ? 1 : 0);
    return 1;
}

int LuaEngine::l_isUpEdge(lua_State *L) {
    LuaEngine *self = Self(L);
    const int key = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, self && key >= 0 && key < 256 && self->key_up_edge_[key] ? 1 : 0);
    return 1;
}

// e:getMousePoint() → {x=…, y=…} (stage coordinates; scaled by the feeder)
int LuaEngine::l_getMousePoint(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_newtable(L);
    lua_pushnumber(L, self ? self->mouse_x_ : 0.0f);
    lua_setfield(L, -2, "x");
    lua_pushnumber(L, self ? self->mouse_y_ : 0.0f);
    lua_setfield(L, -2, "y");
    return 1;
}

int LuaEngine::l_getTouchCount(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushinteger(L, self ? self->touch_count_ : 0);
    return 1;
}

// e:setEventHandler{onEnterFrame="fn", …} — engine → Lua frame callbacks
int LuaEngine::l_setEventHandler(lua_State *L) {
    LuaEngine *self = Self(L);
    if (!self || !lua_istable(L, 2)) return 0;
    static const char *kNames[] = {"onEnterFrame", "onClickWaitIn", "onClickWaitOut",
                                   "onSave", "onLoad", "onInputEvent"};
    for (const char *n : kNames) {
        lua_getfield(L, 2, n);
        const char *fn = lua_tostring(L, -1);
        if (fn) self->event_handlers_[n] = fn;
        lua_pop(L, 1);
    }
    return 0;
}

// e:overrideKey{key=…, status=…} — key remapping; accepted silently for now
int LuaEngine::l_overrideKey(lua_State *L) {
    (void)L;
    return 0;
}

// e:lyevent{...} — register a layer event handler (click/over/out)
int LuaEngine::l_lyevent(lua_State *L) {
    LuaEngine *self = Self(L);
    if (!self || !lua_istable(L, 2)) return 0;
    lua_getfield(L, 2, "id");
    const char *id = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!id) return 0;
    std::vector<std::pair<std::string, std::string>> attrs;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            const char *v = lua_tostring(L, -1);
            if (k && v) attrs.emplace_back(k, v);
        }
        lua_pop(L, 1);
    }
    // The framework emits multiple same-id lyevent tags per layer (click,
    // rollover, rollout; dragin, drag, dragout) — key them by event type so
    // the slider's drag chain (dragin/dragX/dragout) and button click all
    // survive. Non-click tags only register if no entry exists yet for that type.
    std::string ty;
    for (const auto &kv : attrs)
        if (kv.first == "type") ty = kv.second;
    if (ty.empty()) ty = "click";
    auto &by_type = self->lyevents_[id];
    if (ty != "click" && by_type.count(ty)) return 0;
    by_type[ty] = std::move(attrs);
    return 0;
}

// Locate the effective click/drag attr table for a hit layer, walking up the
// id hierarchy (a child layer's registrations inherit its ancestors').
// When `out` is null, only reports existence (`registered` hit-test).
bool LuaEngine::FindLayerEvent(const std::string &id, const std::string &type,
                               std::vector<std::pair<std::string, std::string>> *out) const {
    std::string cur = id;
    while (true) {
        const auto it = lyevents_.find(cur);
        if (it != lyevents_.end()) {
            const auto t2 = it->second.find(type);
            if (t2 != it->second.end()) {
                if (out) *out = t2->second;
                return true;
            }
        }
        const size_t dot = cur.rfind('.');
        if (dot == std::string::npos) return false;
        cur = cur.substr(0, dot);
    }
}

void LuaEngine::ClickAt(float x, float y) {
    SetWaiting(false);   // a tap ends the current click-wait
    if (!compositor_) return;
    const std::string id = compositor_->HitLayer(x, y);
    Log(kLogInfo, "click: hit='" + id + "' registered=" +
                      (FindLayerEvent(id, "click", nullptr) ? "yes" : "no"));
    if (id.empty()) return;
    std::vector<std::pair<std::string, std::string>> attrs;
    if (!FindLayerEvent(id, "click", &attrs)) return;
    // touch model: rollover sets btn.cursor first, then the click fires.
    for (const auto &kv : attrs)     // over
        if (kv.first == "over" && !kv.second.empty())
            CallEvent(kv.second, attrs, false);
    // Button dispatch follows the original kernel's two-step model:
    //   * if the ACTIVE button def carries `exec`, run it directly (covers
    //     title / language / save-slot buttons — the lyevent click attr);
    //   * otherwise the button only moves the cursor (btn_clickex syncs) and
    //     the action is driven by the CLICK key, which the framework routes
    //     through setonpush_calllua (dialog yes/no, UI key nav, ...).
    std::string exec;
    DoString("local b=btn and btn.cursor; local g=btn and btn.name;"
             "local i=b and g and btn[g] and btn[g].p[b];"
             "_artc_exec=i and i.exec or ''", "exec-query");
    lua_getglobal(L_, "_artc_exec");
    if (lua_isstring(L_, -1)) exec = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    if (!exec.empty()) {
        // A real engine click event carries the pressed button as `btn` —
        // button handlers like langsel_click read p.btn (-> getBtnInfo) to
        // act. Our lyevent attrs only have `key`, so fold the key in.
        std::vector<std::pair<std::string, std::string>> click_attrs = attrs;
        for (const auto &kv : attrs)
            if (kv.first == "key")
                click_attrs.emplace_back("btn", kv.second);
        for (const auto &kv : attrs)
            if (kv.first == "click" && !kv.second.empty())
                CallEvent(kv.second, click_attrs, false);
    } else {
        for (const auto &kv : attrs)     // cursor-sync (function = btn_clickex)
            if (kv.first == "function" && !kv.second.empty())
                CallEvent(kv.second, attrs, true);
        FireOnPush(1);                   // CLICK key → setonpush_calllua
    }
}

// Key press → registered setonpush handler (framework click routing).
void LuaEngine::FireOnPush(int key) {
    const auto it = onpush_.find(key);
    if (it == onpush_.end()) return;
    const auto attrs = it->second;   // copy: handler may re-enter
    for (const auto &kv : attrs)
        if (kv.first == "function" && !kv.second.empty()) {
            CallEvent(kv.second, attrs, false);
            break;
        }
}

// ---- draggable layers (framework slider pins) ----

// key-1 down over a draggable layer: record the grab and fire dragin.
void LuaEngine::BeginDrag(float x, float y) {
    if (drag_id_.empty() && compositor_) {
        const std::string id = compositor_->HitLayer(x, y);
        const auto info = compositor_->GetLayerInfo(id);
        if (info.found && info.draggable) {
            drag_id_ = id;
            drag_origin_x_ = x; drag_origin_y_ = y;
            drag_off_x_ = info.left; drag_off_y_ = info.top;
            Log(kLogInfo, "drag: begin " + id + " off=" +
                              std::to_string((int)info.left) + "," +
                              std::to_string((int)info.top));
            std::vector<std::pair<std::string, std::string>> attrs;
            if (FindLayerEvent(id, "dragin", &attrs))
                for (const auto &kv : attrs)
                    if (kv.first == "function" && !kv.second.empty()) {
                        CallEvent(kv.second, attrs, true);
                        break;
                    }
        }
    }
}

// pointer move: clamp the layer's stored offset to its dragarea and fire
// the drag handler (slider_dragX reads get_layer_info → percent → p4).
void LuaEngine::DragMove(float x, float y) {
    if (drag_id_.empty() || !compositor_) return;
    const auto info = compositor_->GetLayerInfo(drag_id_);
    if (!info.found) { EndDrag(); return; }
    float nx = drag_off_x_ + (x - drag_origin_x_);
    float ny = drag_off_y_ + (y - drag_origin_y_);
    if (info.has_dragarea) {
        if (nx < info.drag_l) nx = info.drag_l;
        else if (nx > info.drag_r) nx = info.drag_r;
        if (ny < info.drag_t) ny = info.drag_t;
        else if (ny > info.drag_b) ny = info.drag_b;
    }
    std::map<std::string, std::string> props;
    props["left"] = std::to_string((int)nx);
    compositor_->SetProps(drag_id_, props);
    std::vector<std::pair<std::string, std::string>> attrs;
    if (FindLayerEvent(drag_id_, "drag", &attrs))
        for (const auto &kv : attrs)
            if (kv.first == "function" && !kv.second.empty()) {
                CallEvent(kv.second, attrs, true);
                break;
            }
}

// key-1 up: fire dragout and clear the grab.
void LuaEngine::EndDrag() {
    if (drag_id_.empty()) return;
    std::vector<std::pair<std::string, std::string>> attrs;
    if (FindLayerEvent(drag_id_, "dragout", &attrs))
        for (const auto &kv : attrs)
            if (kv.first == "function" && !kv.second.empty()) {
                CallEvent(kv.second, attrs, true);
                break;
            }
    Log(kLogInfo, "drag: end " + drag_id_);
    drag_id_.clear();
}

bool LuaEngine::PushGlobalFn(const std::string &fn, bool quiet) {
    size_t start = 0;
    bool first = true;
    for (;;) {
        const size_t dot = fn.find('.', start);
        const std::string part = dot == std::string::npos
                                     ? fn.substr(start) : fn.substr(start, dot - start);
        if (first) lua_getglobal(L_, part.c_str());
        else lua_getfield(L_, -1, part.c_str());
        first = false;
        if (dot == std::string::npos) break;
        if (!lua_istable(L_, -1)) {
            if (!quiet) Log(kLogError, "calllua: global not found: " + fn);
            lua_pop(L_, 1);
            return false;
        }
        start = dot + 1;
    }
    if (!lua_isfunction(L_, -1)) {
        if (!quiet) Log(kLogError, "calllua: global not found: " + fn);
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

// engine -> Lua event invocation: fn(param_table), param = lyevent attrs
void LuaEngine::CallEvent(const std::string &fn,
                          const std::vector<std::pair<std::string, std::string>> &param,
                          bool quiet) {
    if (!L_) return;
    if (!PushGlobalFn(fn, quiet)) return;
    lua_getglobal(L_, kBridgeTable);
    lua_newtable(L_);
    for (const auto &kv : param) {
        lua_pushlstring(L_, kv.first.c_str(), kv.first.size());
        lua_pushlstring(L_, kv.second.c_str(), kv.second.size());
        lua_settable(L_, -3);
    }
    if (lua_pcall(L_, 2, 0, 0) != 0) {
        if (!quiet)
            Log(kLogError, "event " + fn + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }
}

void LuaEngine::SetWaiting(bool w) {
    if (waiting_ == w) return;
    waiting_ = w;
    if (!w) timed_wait_ = false;   // manual release also clears any timed wait
    const auto it = event_handlers_.find(w ? "onClickWaitIn" : "onClickWaitOut");
    if (it != event_handlers_.end() && !it->second.empty())
        CallGlobalInternal(it->second, true);
}

// [wt]/[wait] style time waits: hold the runner until the deadline passes.
void LuaEngine::SetTimedWait(int ms) {
    wait_until_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    timed_wait_ = true;
    SetWaiting(true);
}

// Polled once per frame: auto-clears an expired timed wait so the runner
// resumes without user input. Called by the frame loop before stepping.
bool LuaEngine::IsWaiting() {
    if (timed_wait_ && std::chrono::steady_clock::now() >= wait_until_) {
        timed_wait_ = false;
        SetWaiting(false);
    }
    return waiting_;
}

// [save] persistence: length-prefixed {key,value} pairs in <game>/system.dat.
// Transient t.* variables are skipped. Restored by LoadSystemData before the
// framework's fload_pluto runs (so e:var returns the pluto blobs).
void LuaEngine::SaveSystemData() {
    if (save_dir_.empty()) {
        Log(kLogWarn, "save: no save dir set; skipped");
        return;
    }
    std::string path = save_dir_;
    if (path.back() != '/') path += '/';
    path += "system.dat";
    std::ofstream of(path, std::ios::binary | std::ios::trunc);
    if (!of) {
        Log(kLogWarn, "save: cannot write " + path);
        return;
    }
    std::vector<std::pair<std::string, std::string>> items;
    for (const auto &kv : vars_)
        if (kv.first.rfind("t.", 0) != 0) items.push_back(kv);
    const uint32_t n = static_cast<uint32_t>(items.size());
    of.write(reinterpret_cast<const char *>(&n), sizeof(n));
    for (const auto &kv : items) {
        const uint32_t kl = static_cast<uint32_t>(kv.first.size());
        const uint32_t vl = static_cast<uint32_t>(kv.second.size());
        of.write(reinterpret_cast<const char *>(&kl), sizeof(kl));
        of.write(kv.first.data(), kv.first.size());
        of.write(reinterpret_cast<const char *>(&vl), sizeof(vl));
        of.write(kv.second.data(), kv.second.size());
    }
    Log(kLogInfo, "save: " + std::to_string(items.size()) + " vars -> " + path);
}

void LuaEngine::LoadSystemData() {
    if (save_dir_.empty()) return;
    std::string path = save_dir_;
    if (path.back() != '/') path += '/';
    path += "system.dat";
    std::ifstream in(path, std::ios::binary);
    if (!in) return;   // no save yet — first boot
    uint32_t n = 0;
    if (!in.read(reinterpret_cast<char *>(&n), sizeof(n))) return;
    if (n > 65536) {   // guard against garbage
        Log(kLogWarn, "save: bogus entry count in " + path);
        return;
    }
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t kl = 0, vl = 0;
        if (!in.read(reinterpret_cast<char *>(&kl), sizeof(kl))) break;
        if (kl > 1 << 20) break;
        std::string k(kl, '\0');
        if (!in.read(&k[0], kl)) break;
        if (!in.read(reinterpret_cast<char *>(&vl), sizeof(vl))) break;
        if (vl > 1 << 24) break;
        std::string v(vl, '\0');
        if (!in.read(&v[0], vl)) break;
        vars_[std::move(k)] = std::move(v);
    }
    Log(kLogInfo, "save: restored variables from " + path);
}

// e:getScriptWaitReason() — table whose keys name active non-click waits.
// Official wait reasons are time/textTween/textClearTween/sound/video. A plain
// click wait is signalled by onClickWaitIn/Out and leaves this table empty,
// which matches the adv framework's getWaitStatus() gate.
int LuaEngine::l_getScriptWaitReason(lua_State *L) {
    (void)Self(L);
    lua_newtable(L);
    return 1;
}

// e:random() — uniform [0,1) (official: 无参数 → number). Used by the media
// framework (sysvo voice pick) expecting a float for % arithmetic.
int LuaEngine::l_random(lua_State *L) {
    lua_pushnumber(L, static_cast<double>(rand()) / (RAND_MAX + 1.0));
    return 1;
}

// e:getScriptStack() — the native script stack. The compat runner is flat
// (no subroutine frames yet), so an empty table is the honest answer; the
// framework only counts/length-checks it (#ss, table.maxn).
int LuaEngine::l_getScriptStack(lua_State *L) {
    lua_newtable(L);
    return 1;
}

// e:enqueueTag{"name", k=v, …} — queue a tag for the next engine wait point.
int LuaEngine::l_enqueueTag(lua_State *L) {
    LuaEngine *self = Self(L);
    if (!self || !lua_istable(L, 2)) return 0;
    lua_rawgeti(L, 2, 1);
    const char *name = lua_tostring(L, -1);
    lua_pop(L, 1);
    if (!name) return 0;
    std::vector<std::pair<std::string, std::string>> attrs;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            const char *v = lua_tostring(L, -1);
            if (k && v) attrs.emplace_back(k, v);
        }
        lua_pop(L, 1);
    }
    self->tag_queue_.emplace_back(name, std::move(attrs));
    Log(kLogDebug, std::string("enqueueTag: ") + name);
    return 0;
}

bool LuaEngine::EnqueueTag(const std::string &name,
                           const std::vector<std::pair<std::string, std::string>> &attrs) {
    if (!L_) return false;
    tag_queue_.emplace_back(name, attrs);
    return true;
}

bool LuaEngine::PopQueuedTag(std::string *name,
                             std::vector<std::pair<std::string, std::string>> *attrs) {
    if (tag_queue_.empty()) return false;
    *name = std::move(tag_queue_.front().first);
    *attrs = std::move(tag_queue_.front().second);
    tag_queue_.pop_front();
    return true;
}

// e:include(path) — load + execute a Lua source from the pack chain.
int LuaEngine::l_include(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushlightuserdata(L, reinterpret_cast<void *>(&kPackKey));
    lua_gettable(L, LUA_REGISTRYINDEX);
    PackManager *packs = static_cast<PackManager *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const char *path = luaL_checkstring(L, 2);
    const std::string resolved = self->ResolvePackPath(path);

    std::vector<uint8_t> bytes;
    if (!packs->Read(resolved, bytes)) {
        Log(kLogWarn, "include: not found in packs: " + resolved);
        lua_pushnil(L);
        return 1;
    }
    const int top = lua_gettop(L);
    if (luaL_loadbuffer(L, reinterpret_cast<const char *>(bytes.data()), bytes.size(),
                        resolved.c_str()) != 0) {
        Log(kLogError, std::string("include load error: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
        return 0;
    }
    if (lua_pcall(L, 0, 0, 0) != 0) {
        Log(kLogError, std::string("include run error: ") + lua_tostring(L, -1));
        lua_pop(L, 1);
    }
    lua_settop(L, top);
    return 0;
}

// e:debug(msg)
int LuaEngine::l_debug(lua_State *L) {
    const char *msg = lua_tostring(L, 2);
    Log(kLogInfo, std::string("[Lua] ") + (msg ? msg : ""));
    return 0;
}

// __index(table, key) → closure carrying the member name
int LuaEngine::l_index(lua_State *L) {
    lua_pushvalue(L, 2); // key as upvalue
    lua_pushcclosure(L, l_stub, 1);
    return 1;
}

// no-op bridge members (e.setTagFilter etc.) — accepted silently
int LuaEngine::l_noop(lua_State *L) { return 0; }

// e:now() — playtime in milliseconds since engine init (behavior parity).
int LuaEngine::l_now(lua_State *L) {
    LuaEngine *self = Self(L);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - self->init_time_).count();
    lua_pushinteger(L, static_cast<lua_Integer>(ms));
    return 1;
}

// fallback for unknown e.* members: report each missing API once (M0
// discovery mechanism) — silent no-op afterwards (e.g. unbindSurface is
// called for every deleted layer and would otherwise spam logcat).
int LuaEngine::l_stub(lua_State *L) {
    const char *key = lua_tostring(L, lua_upvalueindex(1));
    if (key) {
        static std::set<std::string> reported;
        if (reported.insert(key).second)
            Log(kLogWarn, std::string("UNIMPLEMENTED: e.") + key);
    }
    return 0;
}

int LuaEngine::l_allkeyoff(lua_State *L) {
    Log(kLogInfo, "allkeyoff: input disabled (stub)");
    return 0;
}

bool LuaEngine::RunPackScript(const std::string &path, std::string *errorOut) {
    std::vector<uint8_t> bytes;
    if (!packs_->Read(path, bytes)) {
        if (errorOut) *errorOut = "not found: " + path;
        return false;
    }
    if (luaL_loadbuffer(L_, reinterpret_cast<const char *>(bytes.data()), bytes.size(),
                        path.c_str()) != 0) {
        if (errorOut) *errorOut = lua_tostring(L_, -1);
        return false;
    }
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        if (errorOut) *errorOut = lua_tostring(L_, -1);
        return false;
    }
    return true;
}

bool LuaEngine::DoString(const std::string &code, const std::string &chunk) {
    if (luaL_loadbuffer(L_, code.c_str(), code.size(), chunk.c_str()) != 0) {
        Log(kLogError, std::string("lua load: ") + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    if (lua_pcall(L_, 0, 0, 0) != 0) {
        Log(kLogError, std::string("lua exec: ") + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

// [calllua function="X"] → call global X with the engine bridge as arg 1.
bool LuaEngine::CallGlobal(const std::string &fn) {
    return CallGlobalInternal(fn, false);
}

bool LuaEngine::CallGlobalInternal(const std::string &fn, bool quiet) {
    if (!L_) return false;
    // dotted names resolve through tables (sv.autosavecheck → sv["autosavecheck"])
    size_t start = 0;
    bool first = true;
    for (;;) {
        const size_t dot = fn.find('.', start);
        const std::string part = dot == std::string::npos
                                     ? fn.substr(start) : fn.substr(start, dot - start);
        if (first) lua_getglobal(L_, part.c_str());
        else lua_getfield(L_, -1, part.c_str());
        first = false;
        if (dot == std::string::npos) break;
        if (!lua_istable(L_, -1)) {
            if (!quiet) Log(kLogError, "calllua: global not found: " + fn);
            lua_pop(L_, 1);
            return false;
        }
        start = dot + 1;
    }
    if (!lua_isfunction(L_, -1)) {
        if (!quiet) Log(kLogError, "calllua: global not found: " + fn);
        lua_pop(L_, 1);
        return false;
    }
    lua_getglobal(L_, kBridgeTable);   // engine bridge passed as arg 1
    if (lua_pcall(L_, 1, 0, 0) != 0) {
        if (!quiet)
            Log(kLogError, "calllua " + fn + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

// Route an engine tag through the e:tag bridge (tag name = array item 1).
bool LuaEngine::DispatchTag(const std::string &tag,
                            const std::vector<std::pair<std::string, std::string>> &attrs) {
    if (!L_) return false;
    lua_getglobal(L_, kBridgeTable);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_getfield(L_, -1, "tag");
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); return false; }
    lua_pushvalue(L_, -2);             // self
    lua_newtable(L_);                  // tag table
    lua_pushlstring(L_, tag.c_str(), tag.size());
    lua_rawseti(L_, -2, 1);
    for (const auto &kv : attrs) {
        lua_pushlstring(L_, kv.first.c_str(), kv.first.size());
        lua_pushlstring(L_, kv.second.c_str(), kv.second.size());
        lua_settable(L_, -3);
    }
    if (lua_pcall(L_, 2, 0, 0) != 0) {
        Log(kLogError, "tag dispatch " + tag + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    lua_pop(L_, 1);                    // pop the e table
    return true;
}

} // namespace artc
