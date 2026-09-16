#include "script/lua_engine.h"
#include "script/asb_parser.h"
#include "script/expression.h"
#include "script/preprocess.h"
#include "render/compositor.h"
#include "render/video_player.h"
#include "render/emote_player.h"
#include "render/stb_image.h"
#include "audio/audio.h"
#include "audio/audio_channels.h"
#include "pack/pack_manager.h"
#include "pack/psb.h"
#include "script/pluto_lua.h"
#include "script/pluto_codec.h"
#include "save/native_save.h"
#include "util/save_storage.h"
#include "save/save_metadata.h"
#include "script/runtime_state.h"
#include "util/encoding.h"
#include <filesystem>
#include "log/logger.h"

#include <cstring>
#include <cctype>
#include <cstdint>
#include <new>       // placement new for the E-mote proxy userdata
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <set>

extern "C" {
#include "lauxlib.h"   // luaL_traceback (debug support); vendored with lua.hpp
}

namespace artc {

namespace {
// KrKr2-Next: lua_pcall message handler that appends debug.traceback so
// framework errors surfaced through the bridge (calllua / lyevent / e:tag)
// show the Lua call chain instead of just the failing line.
int TracebackHandler(lua_State *L) {
    const char *msg = lua_tostring(L, 1);
    lua_getglobal(L, "debug");
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return 1; }
    lua_getfield(L, -1, "traceback");
    if (!lua_isfunction(L, -1)) { lua_pop(L, 2); return 1; }
    lua_pushstring(L, msg ? msg : "(non-string error)");
    lua_pushinteger(L, 2);
    lua_call(L, 2, 1);
    return 1;
}

// lua_pcall with TracebackHandler. The callee and its `nargs` arguments
// must be on top of the stack, exactly as for lua_pcall.
int PCallTraceback(lua_State *L, int nargs, int nresults) {
    const int base = lua_gettop(L) - nargs;   // callee index
    lua_pushcfunction(L, TracebackHandler);
    lua_insert(L, base);                        // handler sits below the callee
    const int rc = lua_pcall(L, nargs, nresults, base);
    lua_remove(L, base);
    return rc;
}
bool ValidateBankGraphs(lua_State* L,const VariableBank& bank,std::string& error) {
    for(const auto& v:bank) {
        if(v.second.size()<4 || v.second.compare(0,4,std::string("\1\0\0\0",4)))continue;
        const int top=lua_gettop(L);lua_getglobal(L,"pluto");lua_getfield(L,-1,"unpersist");
        lua_newtable(L);lua_pushlstring(L,v.second.data(),v.second.size());
        const int rc=lua_pcall(L,2,1,0);
        if(rc) {const char* message=lua_tostring(L,-1);error=v.first+": "+(message?message:"invalid Pluto graph");}
        lua_settop(L,top);if(rc)return false;
    }
    return true;
}
} // namespace


namespace {
const char kBridgeTable[] = "e";
char kEngineKey;
char kPackKey;
} // namespace

// ---- E-mote layer proxies (e:createEmoteLayer) ----
// The userdata holds {engine, layer id}; every method resolves the live
// player through the engine registry, so a proxy surviving a lydel or a
// same-id replace reports "layer removed" instead of dangling. Method
// names follow the reference motion-player binding; the original proxy was
// luabind-registered with the same camelCase surface, so both spellings
// are accepted. Unregistered physics/hit-test methods (startWind,
// setOuterForce, contains, ...) fall through to a logging stub like the
// e-table does — this layer deliberately does not fake SDK behavior it
// cannot render.
namespace {
const char kEmoteMetaName[] = "artc.emote";

struct EmoteProxy {
    LuaEngine *engine;
    std::string id;
};

EmoteProxy *CheckEmoteProxy(lua_State *L) {
    return static_cast<EmoteProxy *>(luaL_checkudata(L, 1, kEmoteMetaName));
}

// Resolve the proxy's live player; on a stale handle leaves
// nil + "E-mote layer removed" on the stack (caller returns 2).
EmotePlayer *Emote(lua_State *L) {
    EmoteProxy *p = CheckEmoteProxy(L);
    if (p) {
        EmotePlayer *e = p->engine->FindEmote(p->id);
        if (e) return e;
    }
    Log(kLogError, "emote: layer removed");
    lua_pushnil(L);
    lua_pushstring(L, "E-mote layer removed");
    return nullptr;
}

// Failable operation: log and return false + reason (2 values).
int EmoteError(lua_State *L, const std::string &error) {
    Log(kLogError, "emote: " + error);
    lua_pushboolean(L, false);
    lua_pushstring(L, error.c_str());
    return 2;
}

int m_setRot(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->SetRot(luaL_checknumber(L, 2), luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}
int m_getRot(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushnumber(L, e->GetRot());
    return 1;
}
int m_setCoord(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->SetCoord(luaL_checknumber(L, 2), luaL_checknumber(L, 3),
                luaL_optnumber(L, 4, 0), luaL_optnumber(L, 5, 0));
    return 0;
}
int m_getCoord(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    double x = 0, y = 0;
    e->GetCoord(&x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
// Reference contract is setScale(s, transition, ease); four args are the
// per-axis extension setScale(sx, sy, transition, ease).
int m_setScale(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const double s = luaL_checknumber(L, 2);
    if (lua_gettop(L) >= 4) {
        e->SetScale(s, luaL_checknumber(L, 3), luaL_optnumber(L, 4, 0),
                    luaL_optnumber(L, 5, 0));
    } else {
        e->SetScale(s, s, luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    }
    return 0;
}
int m_getScale(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    double x = 0, y = 0;
    e->GetScale(&x, &y);
    lua_pushnumber(L, x);
    lua_pushnumber(L, y);
    return 2;
}
int m_setMirror(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->SetMirror(lua_toboolean(L, 2) != 0);
    return 0;
}
int m_setColor(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->SetColor(static_cast<uint32_t>(luaL_checknumber(L, 2)),
                luaL_optnumber(L, 3, 0), luaL_optnumber(L, 4, 0));
    return 0;
}
int m_getColor(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, static_cast<lua_Integer>(e->GetColor()));
    return 1;
}
int m_show(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->Show();
    return 0;
}
int m_hide(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->Hide();
    return 0;
}

int m_playTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->PlayTimeline(label, static_cast<int>(luaL_optinteger(L, 3, 0)), error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_stopTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->StopTimeline(luaL_checkstring(L, 2));
    return 0;
}
int m_isTimelinePlaying(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushboolean(L, e->IsTimelinePlaying(luaL_checkstring(L, 2)) ? 1 : 0);
    return 1;
}
int m_isLoopTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushboolean(L, e->IsLoopTimeline(luaL_checkstring(L, 2)) ? 1 : 0);
    return 1;
}
int m_getTimelineTotalFrameCount(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushnumber(L, e->TimelineTotalFrames(luaL_checkstring(L, 2)));
    return 1;
}
int m_fadeInTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->FadeInTimeline(label, luaL_checknumber(L, 3),
                           static_cast<int>(luaL_optinteger(L, 4, 0)), error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_fadeOutTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->FadeOutTimeline(label, luaL_checknumber(L, 3), error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_setTimelineBlendRatio(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->SetTimelineBlendRatio(label, luaL_checknumber(L, 3), error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_getTimelineBlendRatio(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushnumber(L, e->TimelineBlendRatio(luaL_checkstring(L, 2), nullptr));
    return 1;
}
// setTimeline(label, loop): loop=false parks a looping timeline at its
// loop end instead of wrapping; loop=true resumes wrapping.
int m_setTimeline(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->SetTimelineHoldEnd(label, lua_toboolean(L, 3) != 0, error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_countMainTimelines(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, e->CountMainTimelines());
    return 1;
}
int m_getMainTimelineLabelAt(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushstring(L, e->MainTimelineLabelAt(
                         static_cast<int>(luaL_optinteger(L, 2, -1))).c_str());
    return 1;
}
int m_countDiffTimelines(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, e->CountDiffTimelines());
    return 1;
}
int m_getDiffTimelineLabelAt(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushstring(L, e->DiffTimelineLabelAt(
                         static_cast<int>(luaL_optinteger(L, 2, -1))).c_str());
    return 1;
}
int m_countPlayingTimelines(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, e->CountPlayingTimelines());
    return 1;
}
int m_getPlayingTimelineLabelAt(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushstring(L, e->PlayingTimelineLabelAt(
                         static_cast<int>(luaL_optinteger(L, 2, -1))).c_str());
    return 1;
}
int m_getPlayingTimelineFlagsAt(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, e->PlayingTimelineFlagsAt(
                         static_cast<int>(luaL_optinteger(L, 2, -1))));
    return 1;
}
int m_countVariables(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushinteger(L, e->CountVariables());
    return 1;
}
int m_getVariableLabelAt(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushstring(L, e->VariableLabelAt(
                         static_cast<int>(luaL_optinteger(L, 2, -1))).c_str());
    return 1;
}
int m_setVariable(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    const char *label = luaL_checkstring(L, 2);
    std::string error;
    if (!e->SetVariable(label, luaL_checknumber(L, 3), luaL_optnumber(L, 4, 0),
                        luaL_optnumber(L, 5, 0), error))
        return EmoteError(L, error);
    lua_pushboolean(L, true);
    return 1;
}
int m_getVariable(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    bool found = false;
    const double value = e->GetVariable(luaL_checkstring(L, 2), &found);
    if (!found) return EmoteError(L, std::string("unknown E-mote variable: ") +
                                         luaL_checkstring(L, 2));
    lua_pushnumber(L, value);
    return 1;
}
int m_getAnimating(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    lua_pushboolean(L, e->IsAnimating() ? 1 : 0);
    return 1;
}
int m_skip(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->Skip();
    return 0;
}
int m_pass(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->Pass();
    return 0;
}
int m_progress(lua_State *L) {
    EmotePlayer *e = Emote(L);
    if (!e) return 2;
    e->Progress(luaL_checknumber(L, 2));
    return 0;
}

const struct {
    const char *name;
    lua_CFunction fn;
} kEmoteMethods[] = {
    {"setRot", m_setRot}, {"SetRot", m_setRot},
    {"setRotate", m_setRot}, {"SetRotate", m_setRot},
    {"getRot", m_getRot}, {"GetRot", m_getRot},
    {"setCoord", m_setCoord}, {"SetCoord", m_setCoord},
    {"getCoord", m_getCoord}, {"GetCoord", m_getCoord},
    {"setScale", m_setScale}, {"SetScale", m_setScale},
    {"getScale", m_getScale}, {"GetScale", m_getScale},
    {"setMirror", m_setMirror}, {"SetMirror", m_setMirror},
    {"setColor", m_setColor}, {"SetColor", m_setColor},
    {"getColor", m_getColor}, {"GetColor", m_getColor},
    {"show", m_show}, {"Show", m_show},
    {"hide", m_hide}, {"Hide", m_hide},
    {"playTimeline", m_playTimeline}, {"PlayTimeline", m_playTimeline},
    {"stopTimeline", m_stopTimeline}, {"StopTimeline", m_stopTimeline},
    {"isTimelinePlaying", m_isTimelinePlaying}, {"IsTimelinePlaying", m_isTimelinePlaying},
    {"isLoopTimeline", m_isLoopTimeline}, {"IsLoopTimeline", m_isLoopTimeline},
    {"getTimelineTotalFrameCount", m_getTimelineTotalFrameCount},
    {"GetTimelineTotalFrameCount", m_getTimelineTotalFrameCount},
    {"fadeInTimeline", m_fadeInTimeline}, {"FadeInTimeline", m_fadeInTimeline},
    {"fadeOutTimeline", m_fadeOutTimeline}, {"FadeOutTimeline", m_fadeOutTimeline},
    {"setTimelineBlendRatio", m_setTimelineBlendRatio},
    {"SetTimelineBlendRatio", m_setTimelineBlendRatio},
    {"getTimelineBlendRatio", m_getTimelineBlendRatio},
    {"GetTimelineBlendRatio", m_getTimelineBlendRatio},
    {"setTimeline", m_setTimeline}, {"SetTimeline", m_setTimeline},
    {"countMainTimelines", m_countMainTimelines},
    {"CountMainTimelines", m_countMainTimelines},
    {"getMainTimelineLabelAt", m_getMainTimelineLabelAt},
    {"GetMainTimelineLabelAt", m_getMainTimelineLabelAt},
    {"countDiffTimelines", m_countDiffTimelines},
    {"CountDiffTimelines", m_countDiffTimelines},
    {"getDiffTimelineLabelAt", m_getDiffTimelineLabelAt},
    {"GetDiffTimelineLabelAt", m_getDiffTimelineLabelAt},
    {"countPlayingTimelines", m_countPlayingTimelines},
    {"CountPlayingTimelines", m_countPlayingTimelines},
    {"getPlayingTimelineLabelAt", m_getPlayingTimelineLabelAt},
    {"GetPlayingTimelineLabelAt", m_getPlayingTimelineLabelAt},
    {"getPlayingTimelineFlagsAt", m_getPlayingTimelineFlagsAt},
    {"GetPlayingTimelineFlagsAt", m_getPlayingTimelineFlagsAt},
    {"countVariables", m_countVariables}, {"CountVariables", m_countVariables},
    {"getVariableLabelAt", m_getVariableLabelAt},
    {"GetVariableLabelAt", m_getVariableLabelAt},
    {"setVariable", m_setVariable}, {"SetVariable", m_setVariable},
    {"getVariable", m_getVariable}, {"GetVariable", m_getVariable},
    {"getAnimating", m_getAnimating}, {"GetAnimating", m_getAnimating},
    {"isAnimating", m_getAnimating}, {"IsAnimating", m_getAnimating},
    {"skip", m_skip}, {"Skip", m_skip},
    {"pass", m_pass}, {"Pass", m_pass},
    {"progress", m_progress}, {"Progress", m_progress},
};

// __index(userdata, key) — registered method, else a logging stub closure
// (same discovery pattern as the e-table's l_index/l_stub pair).
int EmoteStub(lua_State *L);

int EmoteIndex(lua_State *L) {
    if (lua_type(L, 2) != LUA_TSTRING) {
        lua_pushnil(L);
        return 1;
    }
    const char *key = lua_tostring(L, 2);
    for (const auto &m : kEmoteMethods)
        if (std::strcmp(m.name, key) == 0) {
            lua_pushcfunction(L, m.fn);
            return 1;
        }
    lua_pushvalue(L, 2);
    lua_pushcclosure(L, EmoteStub, 1);
    return 1;
}

int EmoteStub(lua_State *L) {
    static std::set<std::string> reported;
    const char *key = lua_tostring(L, lua_upvalueindex(1));
    if (key && reported.insert(key).second)
        Log(kLogWarn, std::string("UNIMPLEMENTED: emote:") + key);
    return 0;
}

int EmoteGc(lua_State *L) {
    EmoteProxy *p = static_cast<EmoteProxy *>(lua_touserdata(L, 1));
    if (p) p->~EmoteProxy();  // std::string member
    return 0;
}

void PushEmoteProxy(lua_State *L, LuaEngine *engine, const std::string &id) {
    if (luaL_newmetatable(L, kEmoteMetaName)) {
        lua_pushcfunction(L, EmoteGc);
        lua_setfield(L, -2, "__gc");
        lua_pushcfunction(L, EmoteIndex);
        lua_setfield(L, -2, "__index");
    }
    lua_pop(L, 1);  // cached in the registry
    auto *p = static_cast<EmoteProxy *>(lua_newuserdata(L, sizeof(EmoteProxy)));
    new (p) EmoteProxy{engine, id};
    luaL_getmetatable(L, kEmoteMetaName);
    lua_setmetatable(L, -2);
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
    videos_.clear(); // players release their output before Audio is destroyed
    emotes_.clear();
    delete sounds_;
    sounds_ = nullptr;
    if (audio_) { delete audio_; audio_ = nullptr; }
    if (L_) lua_close(L_);
}
void LuaEngine::PauseAudio() { if (audio_) audio_->PauseAll(); }

std::chrono::steady_clock::time_point LuaEngine::ClockNow() const {
    return clock_paused_ ? clock_pause_at_ : std::chrono::steady_clock::now();
}

void LuaEngine::PauseClock() {
    if (clock_paused_) return;
    clock_paused_ = true;
    clock_pause_at_ = std::chrono::steady_clock::now();
}

void LuaEngine::ResumeClock() {
    if (!clock_paused_) return;
    const auto dt = std::chrono::steady_clock::now() - clock_pause_at_;
    init_time_ += dt;
    wait_until_ += dt;
    if (script_runner_) script_runner_->ShiftWaitDeadlines(*this, dt);
    clock_paused_ = false;
}
void LuaEngine::ResumeAudio() { if (audio_) audio_->ResumeAll(); }

// ---- input & frame hooks ----

void LuaEngine::PushKeyDown(int key) { input_.Press(key); }
void LuaEngine::PushKeyUp(int key) { input_.Release(key); }

void LuaEngine::SetMousePoint(float x, float y) { mouse_x_ = x; mouse_y_ = y; HoverMove(x, y); }
void LuaEngine::SetTouchCount(int count) { touch_count_ = count; }

void LuaEngine::EndFrame() {
    input_.EndFrame();
}

bool LuaEngine::RunEnterFrame() {
    ++frame_number_;
    advanced_this_frame_ = false;
    if (sounds_) sounds_->Update(NowMs());
    UpdateVideos();
    UpdateEmotes();
    PollSoundFinish();
    const auto it = event_handlers_.find("onEnterFrame");
    bool ok=true;
    if (it != event_handlers_.end() && !it->second.empty()) {
        const bool quiet=enterframe_failures_>0 && enterframe_failures_%600!=0;
        ok=CallGlobalInternal(it->second,quiet);
        if (ok) enterframe_failures_=0;
        else ++enterframe_failures_;
    }
    DispatchFrameInput();
    return ok;
}

// Standard-library harden: keep the scripting surface the frameworks actually
// use (os.date/io.open/io.close) but drop the process-level host surface and
// the arbitrary-code loaders. ARTC_LUA_STDLIB=stock skips it for A/B tests.
namespace {
void DropFields(lua_State *L, const char *table, const char *const *names, size_t n) {
    lua_getglobal(L, table);
    if (!lua_istable(L, -1)) { lua_pop(L, 1); return; }
    for (size_t i = 0; i < n; ++i) { lua_pushnil(L); lua_setfield(L, -2, names[i]); }
    lua_pop(L, 1);
}
void ApplyLuaStdlibBlockList(lua_State *L) {
    if (const char *v = std::getenv("ARTC_LUA_STDLIB"); v && std::strcmp(v, "stock") == 0)
        return;
    static const char *const kOsBanned[] = {"execute", "exit", "remove", "rename",
                                            "setlocale", "tmpname", "system"};
    DropFields(L, "os", kOsBanned, sizeof(kOsBanned) / sizeof(kOsBanned[0]));
    static const char *const kIoBanned[] = {"popen", "tmpfile", "input", "output"};
    DropFields(L, "io", kIoBanned, sizeof(kIoBanned) / sizeof(kIoBanned[0]));
    // NOTE: loadstring stays reachable — the pluto save codec (src/script/
    // pluto.lua) reconstructs persisted functions through it. dofile/loadfile
    // (host file loaders) are still dropped.
    static const char *const kGlobalBanned[] = {"dofile", "loadfile"};
    for (const char *n : kGlobalBanned) { lua_pushnil(L); lua_setglobal(L, n); }
    static const char *const kPkgBanned[] = {"loadlib", "loaders", "preload", "seeall"};
    DropFields(L, "package", kPkgBanned, sizeof(kPkgBanned) / sizeof(kPkgBanned[0]));
}
} // namespace

bool LuaEngine::Init(PackManager *packs, const Ini &systemIni,
                     const std::string &osName, int screenWidth, int screenHeight,
                     Compositor *compositor) {
    packs_ = packs;
    compositor_ = compositor;
    if(compositor_)compositor_->SetSaveDirectory(save_dir_);
    audio_ = new Audio();
    audio_->Init(packs);
    sounds_ = new AudioChannels(*audio_);
    // Optional project tag.ini: positional parameter names for line tags.
    {
        std::vector<uint8_t> tag_ini;
        if ((packs && packs->Read("tag.ini", tag_ini)) ||
            (packs && packs->Read("system/tag.ini", tag_ini)))
            InstallTagIniFromText(std::string(tag_ini.begin(), tag_ini.end()));
        else
            ClearTagIni();
    }
    // Project text charset (system.ini CHARSET) for script decoding.
    {
        std::string cs = systemIni.Get("WINDOWS", "CHARSET");
        if (cs.empty()) cs = systemIni.Get("ANDROID", "CHARSET");
        SetTextCharset(cs);
    }
    L_ = luaL_newstate();
    if (!L_) return false;
    init_time_ = std::chrono::steady_clock::now();
    luaL_openlibs(L_);
    ApplyLuaStdlibBlockList(L_);
    // register the pluto serializer (save/load data format)
    if (luaL_dostring(L_, PLUTO_LUA_SRC) != 0) {
        Log(kLogError, std::string("pluto registration failed: ") + lua_tostring(L_, -1));
        lua_pop(L_, 1);
    }

    RegisterPlutoCodec(L_);

    // expose engine instance + packs to the C closures via the registry
    lua_pushlightuserdata(L_, reinterpret_cast<void *>(&kEngineKey));
    lua_pushlightuserdata(L_, this);
    lua_settable(L_, LUA_REGISTRYINDEX);
    lua_pushlightuserdata(L_, reinterpret_cast<void *>(&kPackKey));
    lua_pushlightuserdata(L_, packs);
    lua_settable(L_, LUA_REGISTRYINDEX);

    // engine settings used by the `var` tag
    sysvals_["os"] = osName;
    sysvals_["savepath"] = save_dir_;
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
        {"setTagFilter", l_setTagFilter},
        {"setMagicPath", l_setMagicPath},
        {"setUseMultiTouch", l_noop},
        {"setUseTouchHold", l_noop},
        {"random", l_random},
        {"setEventFilter", l_setEventFilter},
        {"setEventHandler", l_setEventHandler},
        {"enqueueTag", l_enqueueTag},
        {"bindSurfaceAsync", l_noop},
        {"isDown", l_isDown},
        {"isPush", l_isPush},
        {"isDecide", l_isDecide},
        {"isDownEdge", l_isDownEdge},
        {"isUpEdge", l_isUpEdge},
        {"getMousePoint", l_getMousePoint},
        {"getTouchCount", l_getTouchCount},
        {"overrideKey", l_overrideKey},
        {"lyevent", l_lyevent},
        {"getScriptStack", l_getScriptStack},
        {"getScriptWaitReason", l_getScriptWaitReason},
        {"getScriptStatus", l_getScriptStatus},
        {"setScriptStatus", l_setScriptStatus},
        {"getScriptSize", l_getScriptSize},
        {"getFrameNumber", l_getFrameNumber},
        {"getTouchPoint", l_getTouchPoint},
        {"setFlickSensitivity", l_setFlickSensitivity},
        {"getScriptBlock", l_getScriptBlock},
        {"bindSurface", l_noop},
        {"clearSurfaceLoadQueue", l_noop},
        {"isLoadingSurface", l_noop},
        // KrKr2-Next: surface cache release is a no-op without a surface
        // cache; PNG text chunks carry the face-part anchors (image_fg.lua
        // getfgfilepos → "pos,x,y[,w,h,frames,com]").
        {"unbindSurface", l_noop},
        {"loadPngComments", l_loadPngComments},
        {"createEmoteLayer", l_createEmoteLayer},
        {"getEmoteLayer", l_getEmoteLayer},
        {"getEmoteVersion", l_getEmoteVersion},
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
    // movie_play crashes at `local p = scr.movie; p.file`. Skip only this
    // incomplete save restoration; a valid movie request plays normally.
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

LuaEngine::LuaEngine() = default;


// e:var(name) — script vars first, then engine system values (s.*), else ""
std::string LuaEngine::ResolveValue(const std::string& value) const {
    if(value.empty() || value.front()!='$') return value;
    std::string result;
    const auto variable=[this](const std::string& name) {
        const auto found=vars_.find(name);
        if(found!=vars_.end()) return found->second;
        const auto key=name.compare(0,2,"s.")==0 ? name.substr(2) : name;
        const auto system=sysvals_.find(key);
        return system!=sysvals_.end() ? system->second : std::string("0");
    };
    if(EvaluateExpression(value.substr(1),variable,result)) return result;
    Log(kLogWarn,"invalid Artemis expression: "+value);
    return "0";
}

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
        lua_pushlstring(L, it->second.data(),it->second.size());
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
    if ((!packs || !packs->Read(resolved, bytes)) && !ReadSaveFile(SavePath(self->save_dir_,resolved),bytes)) {
        Log(kLogWarn, "file: not found in packs or save directory: " + resolved);
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
    bool exists=packs && packs->Exists(self->ResolvePackPath(path));
    if(!exists) {
        const auto save=SavePath(self->save_dir_,path);std::error_code error;
        exists=!save.empty() && std::filesystem::is_regular_file(save,error);
    }
    lua_pushboolean(L,exists);
    return 1;
}

// e:loadPngComments(path) — KrKr2-Next addition. Returns a table mapping each
// PNG text-chunk keyword to its text (tEXt and uncompressed iTXt; zTXt is
// skipped), or nil when the file is missing / not a PNG. The adv framework
// reads `comment` = "pos,x,y[,w,h,frames,com]" from face-part sprites to
// anchor them on the body (image_fg.lua getfgfilepos); without it every face
// lands at 0,0 and vanishes above the body's visible area.
int LuaEngine::l_loadPngComments(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushlightuserdata(L, reinterpret_cast<void *>(&kPackKey));
    lua_gettable(L, LUA_REGISTRYINDEX);
    PackManager *packs = static_cast<PackManager *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    const char *path = luaL_checkstring(L, 2);
    std::string resolved = self->ResolvePackPath(path);
    std::vector<uint8_t> bytes;
    bool ok = packs && packs->Read(resolved, bytes);
    if (!ok && packs) {   // scripts may omit the extension
        resolved += ".png";
        ok = packs->Read(resolved, bytes);
    }
    static const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    if (!ok || bytes.size() < 8 || std::memcmp(bytes.data(), kSig, 8) != 0) {
        lua_pushnil(L);
        return 1;
    }
    lua_newtable(L);
    size_t i = 8;
    int found = 0;
    while (i + 12 <= bytes.size()) {
        const uint32_t len = (uint32_t(bytes[i]) << 24) | (uint32_t(bytes[i + 1]) << 16) |
                             (uint32_t(bytes[i + 2]) << 8) | uint32_t(bytes[i + 3]);
        const std::string type(reinterpret_cast<const char *>(&bytes[i + 4]), 4);
        const size_t data = i + 8;
        if (data + len > bytes.size()) break;
        if (type == "tEXt" || type == "iTXt") {
            const uint8_t *p = &bytes[data];
            size_t k = 0;
            while (k < len && p[k] != 0) ++k;
            const std::string key(reinterpret_cast<const char *>(p), k);
            std::string text;
            if (type == "tEXt") {
                if (k < len) text.assign(reinterpret_cast<const char *>(p + k + 1), len - k - 1);
            } else if (k + 2 < len && p[k + 1] == 0) {   // iTXt, compression flag 0
                // keyword\0 flag method language\0 translated\0 text
                size_t q = k + 3;
                while (q < len && p[q] != 0) ++q;   // language tag
                ++q;
                while (q < len && p[q] != 0) ++q;   // translated keyword
                ++q;
                if (q <= len) text.assign(reinterpret_cast<const char *>(p + q), len - q);
            }
            if (!key.empty()) {
                lua_pushlstring(L, text.data(), text.size());
                lua_setfield(L, -2, key.c_str());
                ++found;
            }
        }
        if (type == "IEND" || type == "IDAT") break;   // text chunks precede image data here
        i = data + len + 4;   // + CRC
    }
    if (found == 0) {
        lua_pop(L, 1);
        lua_pushnil(L);
    }
    return 1;
}

// e:createEmoteLayer{id=…, files={…}, width=…, height=…, progress=…} — load a
// PSB E-mote model as a layer subtree rooted at the id. The original engine
// accepted a multi-file archive (split PSB); this port reads exactly one PSB
// file and fails explicitly otherwise. width/height/progress are accepted for
// call-compatibility (render size is the compositor stage's).
int LuaEngine::l_createEmoteLayer(lua_State *L) {
    LuaEngine *self = Self(L);
    luaL_checktype(L, 2, LUA_TTABLE);
    std::string id;
    std::string file;
    int files = 0;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            if (k && std::strcmp(k, "id") == 0 && lua_type(L, -1) == LUA_TSTRING) {
                id = lua_tostring(L, -1);
            } else if (k && std::strcmp(k, "files") == 0 && lua_istable(L, -1)) {
                // rawgeti walk (no lua_objlen): works on both Lua 5.1 and 5.4
                for (int i = 1;; ++i) {
                    lua_rawgeti(L, -1, i);
                    if (lua_isnil(L, -1)) {
                        lua_pop(L, 1);
                        break;
                    }
                    if (lua_type(L, -1) == LUA_TSTRING) {
                        ++files;
                        if (files == 1) file = lua_tostring(L, -1);
                    }
                    lua_pop(L, 1);
                }
            }
            // width / height / progress: contract parity only (see above)
        }
        lua_pop(L, 1);
    }
    if (id.empty()) {
        Log(kLogError, "createEmoteLayer: missing id");
        lua_pushnil(L);
        return 1;
    }
    if (!self || !self->compositor_) {
        Log(kLogError, "createEmoteLayer: no compositor");
        lua_pushnil(L);
        return 1;
    }
    if (files == 0) {
        Log(kLogError, "createEmoteLayer: missing files");
        lua_pushnil(L);
        return 1;
    }
    if (files > 1) {
        Log(kLogError, "createEmoteLayer: multi-file E-mote archives are not supported");
        lua_pushnil(L);
        return 1;
    }
    std::vector<uint8_t> bytes;
    const std::string resolved = self->ResolvePackPath(file);
    if (!self->packs_ || !self->packs_->Read(resolved, bytes)) {
        Log(kLogError, "createEmoteLayer: file not found: " + resolved);
        lua_pushnil(L);
        return 1;
    }
    PsbDocument document;
    std::string error;
    if (!DecodePsb(bytes, document, error)) {
        Log(kLogError, "createEmoteLayer: " + error + ": " + resolved);
        lua_pushnil(L);
        return 1;
    }
    auto model = std::make_shared<EmoteModel>();
    if (!model->Load(std::move(document), error)) {
        Log(kLogError, "createEmoteLayer: " + error);
        lua_pushnil(L);
        return 1;
    }
    auto player = std::make_unique<EmotePlayer>();
    if (!player->Load(std::move(model), error)) {
        Log(kLogError, "createEmoteLayer: " + error);
        lua_pushnil(L);
        return 1;
    }
    // Replace-in-place: tear the previous same-id layer down first so a stale
    // scene never renders beside the new one.
    const auto old = self->emotes_.find(id);
    if (old != self->emotes_.end()) {
        old->second->RemoveLayers(*self->compositor_, id);
        self->compositor_->DeleteLayer(id);
        self->emotes_.erase(old);
    }
    self->emotes_[id] = std::move(player);
    PushEmoteProxy(L, self, id);
    return 1;
}

// e:getEmoteLayer(id) — proxy for a live E-mote layer, nil when absent.
int LuaEngine::l_getEmoteLayer(lua_State *L) {
    LuaEngine *self = Self(L);
    const char *id = luaL_checkstring(L, 2);
    if (!self || !self->FindEmote(id)) {
        lua_pushnil(L);
        return 1;
    }
    PushEmoteProxy(L, self, id);
    return 1;
}

// e:getEmoteVersion() — SDK version string advertised to the framework.
int LuaEngine::l_getEmoteVersion(lua_State *L) {
    lua_pushstring(L, "3.9.8");
    return 1;
}

EmotePlayer *LuaEngine::FindEmote(const std::string &id) {
    const auto it = emotes_.find(id);
    return it == emotes_.end() ? nullptr : it->second.get();
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
    lua_pushboolean(L, self && self->input_.Query(key, InputState::Down));
    return 1;
}

int LuaEngine::l_isDownEdge(lua_State *L) {
    LuaEngine *self = Self(L);
    const int key = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, self && self->input_.Query(key, InputState::DownEdge));
    return 1;
}

int LuaEngine::l_isUpEdge(lua_State *L) {
    LuaEngine *self = Self(L);
    const int key = static_cast<int>(luaL_checkinteger(L, 2));
    lua_pushboolean(L, self && self->input_.Query(key, InputState::UpEdge));
    return 1;
}

// e:getMousePoint() → {x=…, y=…} (stage coordinates; scaled by the feeder)
int LuaEngine::l_isPush(lua_State *L) {
    auto* self=Self(L);
    lua_pushboolean(L,self && self->input_.Query(luaL_checkinteger(L,2),InputState::Push));
    return 1;
}

int LuaEngine::l_isDecide(lua_State *L) {
    auto* self=Self(L);
    lua_pushboolean(L,self && self->input_.Query(luaL_checkinteger(L,2),InputState::Decide));
    return 1;
}

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

// Overrides affect this frame only; missing key applies to all 320 keys.
int LuaEngine::l_overrideKey(lua_State *L) {
    auto* self=Self(L);
    if (!self || !lua_istable(L,2)) return 0;
    lua_getfield(L,2,"key");
    const int key=lua_isnil(L,-1) ? -1 : luaL_checkinteger(L,-1);
    lua_pop(L,1);
    lua_getfield(L,2,"status");
    const int status=lua_isnil(L,-1) ? -1 : luaL_checkinteger(L,-1);
    lua_pop(L,1);
    self->input_.Override(key,status);
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
    pending_click_=true;click_x_=x;click_y_=y;
}

void LuaEngine::AdvanceByInput() {
    if (advanced_this_frame_) return;
    advanced_this_frame_ = true;
    if (auto_enabled_ && auto_stop_click_) { SetAutoMode(false); return; }
    if (wait_accept_input_ && compositor_ && compositor_->FinishText(NowMs())) return;
    if (wait_accept_input_) SetWaiting(false);
}

void LuaEngine::DispatchFrameInput() {
    const bool click=pending_click_ ||
        (input_.Overridden(1) && input_.Query(1,InputState::Decide));
    const float x=pending_click_ ? click_x_ : mouse_x_;
    const float y=pending_click_ ? click_y_ : mouse_y_;
    pending_click_=false;
    if(!video_wait_.empty()) {
        const bool movie_click=click && (!input_.Overridden(1) || input_.Query(1,InputState::Decide));
        bool cancel=false;
        if(video_skip_==1) cancel=movie_click || input_.Query(13,InputState::Decide);
        else if(video_skip_==2) {
            const auto role=key_roles_.find(1);
            if(role==key_roles_.end()) cancel=movie_click || input_.Query(27,InputState::Decide);
            else for(int key:role->second)
                if(input_.Query(key,InputState::Decide) ||
                   (key==1 && movie_click)) cancel=true;
        }
        if(cancel) videos_.erase(video_wait_);
        return;
    }
    if (click && (!input_.Overridden(1) || input_.Query(1,InputState::Decide)))
        DispatchClick(x,y);
    // Key callbacks can alter their registrations, enqueue scripts, or emit
    // another virtual key. Iterate key ids, then evaluate the click role once.
    for (int key=2;key<InputState::Count;++key) {
        const auto it=onpush_.find(key);
        if (it==onpush_.end()) continue;
        bool repeat=false;
        for(const auto& kv:it->second) if(kv.first=="keyrepeat") repeat=kv.second=="1";
        if (input_.Query(key,InputState::Decide) || (repeat && input_.Query(key,InputState::Push)))
            FireOnPush(key);
    }
    const auto role=key_roles_.find(0);
    if(role!=key_roles_.end()) {
        for(int key:role->second) if(input_.Query(key,InputState::Decide)) {
            AdvanceByInput();break;
        }
    } else if(input_.Query(13,InputState::Decide)) AdvanceByInput();
}

void LuaEngine::DispatchClick(float x, float y) {
    if (!compositor_) {
        if (onpush_.count(1)) FireOnPush(1);
        else AdvanceByInput();
        return;
    }
    // KrKr2-Next: the real engine dispatches a click to the frontmost layer
    // that OWNS a click event (walking up its id chain), not to whatever
    // decorative child happens to be drawn on top of it — the choice-button
    // text layer `1.80.120.N.0.0.2` sits above the button image `.0` that
    // carries the lyevent, and tapping the text must still pick the choice.
    std::string id;
    for (const std::string &cand : compositor_->HitLayers(x, y)) {
        if (FindLayerEvent(cand, "click", nullptr)) { id = cand; break; }
    }
    if (id.empty()) id = compositor_->HitLayer(x, y);
    Log(kLogInfo, "click: hit='" + id + "' registered=" +
                      (FindLayerEvent(id, "click", nullptr) ? "yes" : "no"));
    // [link] message text: clicking the message layer follows the link.
    if (link_active_ && link_enabled_ && !msg_layer_.empty() && !link_label_.empty() &&
        (id == msg_layer_ || id.rfind(msg_layer_ + ".", 0) == 0)) {
        const std::string file = !link_file_.empty()
            ? link_file_
            : (script_runner_ ? script_runner_->CurrentFile() : std::string());
        if (jump_handler_) jump_handler_(file, link_label_);
        return;
    }
    std::vector<std::pair<std::string, std::string>> attrs;
    if (id.empty() || !FindLayerEvent(id, "click", &attrs)) {
        // No button under the pointer: clear the framework's active-button
        // cursor before routing the CLICK key. keyconfig only closes stateful
        // UI (e.g. the volume slider's mwmute branch) when `func` is nil, and
        // a stale cursor otherwise keeps the click bound to the last button.
        DoString("if btn then btn.cursor=nil end", "clear-btn-cursor");
        if (onpush_.count(1)) { Log(kLogInfo, "click: fallback -> onpush key 1"); FireOnPush(1); }
        else { Log(kLogInfo, "click: fallback -> advance"); AdvanceByInput(); }
        return;
    }
    if (!FilterEvent("lyevent", attrs)) return;
    // Button events run above the scenario, which keeps its cursor and wait.
    // A plain click outside an event releases the wait in the branch above.
    const uint64_t event = script_runner_ ? script_runner_->BeginEvent(*this) : 0;
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
        Log(kLogInfo, "click: button exec path id='" + id + "' exec='" + exec + "'");
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
        Log(kLogInfo, "click: button cursor-sync path id='" + id + "' -> onpush key 1");
        for (const auto &kv : attrs)     // cursor-sync (function = btn_clickex)
            if (kv.first == "function" && !kv.second.empty())
                CallEvent(kv.second, attrs, true);
        FireOnPush(1);                   // CLICK key → setonpush_calllua
    }
    if (script_runner_) script_runner_->EndEvent(event);
}

// Key press → registered setonpush handler (framework click routing).
void LuaEngine::FireOnPush(int key) {
    const auto it = onpush_.find(key);
    if (it == onpush_.end()) return;
    const auto attrs = it->second;   // copy: handler may re-enter
    if (!FilterEvent("setonpush", attrs)) return;
    const uint64_t event=script_runner_ ? script_runner_->BeginEvent(*this) : 0;
    for (const auto &kv : attrs)
        if (kv.first == "function" && !kv.second.empty()) {
            Log(kLogInfo, "onpush: key=" + std::to_string(key) + " -> " + kv.second);
            CallEvent(kv.second, attrs, false);
            break;
        }
    if (script_runner_) script_runner_->EndEvent(event);
}

// ---- draggable layers (framework slider pins) ----

// Hover model: the topmost layer under the pointer that owns a rollover event
// gets its `over`/`function` handler; the previous layer gets its rollout
// handler. The tablet dock arms its slide from tab_over, so a pointer entering
// the handle must dispatch rollover or the bar never opens.
void LuaEngine::HoverMove(float x, float y) {
    if (!compositor_) return;
    std::string id;
    for (const std::string &cand : compositor_->HitLayers(x, y))
        if (FindLayerEvent(cand, "rollover", nullptr)) { id = cand; break; }
    if (id == hover_id_) return;
    auto fire = [&](const std::string &layer, const char *type, const char *alias) {
        std::vector<std::pair<std::string, std::string>> attrs;
        if (layer.empty() || !FindLayerEvent(layer, type, &attrs)) return;
        for (const auto &kv : attrs)
            if ((kv.first == "function" || kv.first == alias) && !kv.second.empty()) {
                Log(kLogInfo, std::string("hover: ") + type + " " + layer + " -> " + kv.second);
                CallEvent(kv.second, attrs, true);
                break;
            }
    };
    if (!hover_id_.empty()) fire(hover_id_, "rollout", "out");
    hover_id_ = id;
    if (!id.empty()) fire(id, "rollover", "over");
}

// key-1 down over a draggable layer: record the grab and fire dragin.
void LuaEngine::BeginDrag(float x, float y) {
    if (drag_id_.empty() && compositor_) {
        const std::string id = compositor_->HitLayer(x, y);
        const auto info = compositor_->GetLayerInfo(id);
        if (info.found && info.draggable) {
            drag_id_ = id;
            drag_moved_ = false;
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
    drag_moved_ = true;
    const auto info = compositor_->GetLayerInfo(drag_id_);
    if (!info.found) { EndDrag(); return; }
    float dx, dy;
    if (!compositor_->ParentDelta(drag_id_,x-drag_origin_x_,y-drag_origin_y_,&dx,&dy)) return;
    float nx = drag_off_x_ + dx;
    float ny = drag_off_y_ + dy;
    if (info.has_dragarea) {
        if (nx < info.drag_l) nx = info.drag_l;
        else if (nx > info.drag_r) nx = info.drag_r;
        if (ny < info.drag_t) ny = info.drag_t;
        else if (ny > info.drag_b) ny = info.drag_b;
    }
    std::map<std::string, std::string> props;
    props["left"] = std::to_string((int)nx);
    props["top"] = std::to_string((int)ny);
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
        else {
            lua_getfield(L_, -1, part.c_str());
            lua_remove(L_, -2); // retain the child, not every parent table
        }
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
int LuaEngine::l_setEventFilter(lua_State* L) {
    auto* self = Self(L);
    if (!lua_isnoneornil(L, 2) && !lua_isfunction(L, 2))
        return luaL_error(L, "setEventFilter expects a function or nil");
    luaL_unref(L, LUA_REGISTRYINDEX, self->event_filter_ref_);
    if (lua_isnoneornil(L, 2)) lua_pushnil(L);
    else lua_pushvalue(L, 2);
    self->event_filter_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

bool LuaEngine::FilterEvent(const std::string& kind,
                           const std::vector<std::pair<std::string, std::string>>& attrs) {
    if (event_filter_ref_ < 0) return true;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, event_filter_ref_);
    lua_getglobal(L_, kBridgeTable);
    lua_pushlstring(L_, kind.data(), kind.size());
    lua_newtable(L_);
    for (const auto& kv : attrs) {
        lua_pushlstring(L_, kv.second.data(), kv.second.size());
        lua_setfield(L_, -2, kv.first.c_str());
    }
    if (PCallTraceback(L_, 3, 1) != 0) {
        Log(kLogError, "event filter: " + std::string(lua_tostring(L_, -1)));
        lua_pop(L_, 1);
        return false;
    }
    const int result = lua_isnumber(L_, -1) ? int(lua_tointeger(L_, -1)) : 0;
    lua_pop(L_, 1);
    return result == 0;
}

bool LuaEngine::CallEvent(const std::string &fn,
                          const std::vector<std::pair<std::string, std::string>> &param,
                          bool quiet) {
    if (!L_) return false;
    if (!PushGlobalFn(fn, quiet)) return false;
    lua_getglobal(L_, kBridgeTable);
    lua_newtable(L_);
    for (const auto &kv : param) {
        lua_pushlstring(L_, kv.first.c_str(), kv.first.size());
        lua_pushlstring(L_, kv.second.c_str(), kv.second.size());
        lua_settable(L_, -3);
    }
    if (PCallTraceback(L_, 2, 0) != 0) {
        if (!quiet)
            Log(kLogError, "event " + fn + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

void LuaEngine::SetAutoMode(bool enabled) {
    if (enabled && !auto_allowed_) return;
    if (auto_enabled_ == enabled) return;
    auto_enabled_ = enabled;
    sysvals_["status.automode"] = enabled ? "1" : "0";
    auto_timer_.Reset();
    const auto it = auto_events_.find(enabled ? "onautomodein" : "onautomodeout");
    if (it == auto_events_.end()) return;
    const auto attrs = it->second; // callback can unregister itself
    std::map<std::string, std::string> values(attrs.begin(), attrs.end());
    const auto token = script_runner_ ? script_runner_->BeginEvent(*this) : 0;
    if (!values["function"].empty()) CallEvent(values["function"], attrs, false);
    else if (!values["file"].empty()) DispatchTag(values["handler"] == "jump" ? "jump" : "call", attrs);
    if (script_runner_) script_runner_->EndEvent(token);
}

void LuaEngine::FireNamedEvent(const std::string &key) {
    const auto it = named_events_.find(key);
    if (it == named_events_.end()) return;
    const auto attrs = it->second; // callback can unregister itself
    std::map<std::string, std::string> values(attrs.begin(), attrs.end());
    const auto token = script_runner_ ? script_runner_->BeginEvent(*this) : 0;
    if (!values["function"].empty()) CallEvent(values["function"], attrs, false);
    else if (!values["file"].empty())
        DispatchTag(values["handler"] == "jump" ? "jump" : "call", attrs);
    if (script_runner_) script_runner_->EndEvent(token);
}

void LuaEngine::SetWaiting(bool w) {
    if (waiting_ != w) auto_timer_.Reset();
    // Kernel announce protocol for onClickWaitIn/Out. keyClickStart records
    // flg.waitflag = getWaitStatus() when a wait begins, and keyClickEnd
    // drops it only while getScriptWaitReason() still reports a reason
    // (vsync.lua: "if getWaitStatus() then flg.waitflag = nil end"). OUT is
    // therefore announced before the wait state is cleared, IN after it is
    // set (every start path sets its reason flag first and calls here
    // last). IN covers all waits; a plain click wait carries no reason, so
    // flg.waitflag stays nil and dialogue clicks keep routing through
    // flg.click. Announcing after the state was gone left a stale
    // flg.waitflag — the framework stayed in "wait" mode forever and every
    // later click was consumed as a dummy exclick (title buttons dead).
    if (waiting_ && !w) AnnounceWaitState(false);
    if (!w) { se_wait_ = false; timed_wait_ = false; }
    else if (!timed_wait_ && !se_wait_) wait_accept_input_ = true;
    waiting_ = w;
    if (w) AnnounceWaitState(true);
}

void LuaEngine::AnnounceWaitState(bool in_wait) {
    if (click_wait_announced_ == in_wait) return;
    click_wait_announced_ = in_wait;
    const auto it = event_handlers_.find(in_wait ? "onClickWaitIn" : "onClickWaitOut");
    if (it != event_handlers_.end() && !it->second.empty()) {
        // The handler calls e:getScriptWaitReason() to decide whether to drop
        // flg.waitflag. That call must observe the pre-transition reason, so
        // its lazy IsWaiting() poll (l_getScriptWaitReason line 1) is
        // suppressed: re-entering IsWaiting() here would run SetWaiting(false)
        // again and clear timed_wait_/se_wait_/waiting_ mid-announce, leaving
        // an empty reason and the stale flg.waitflag this protocol exists to
        // clear. The poll's caller (the outer IsWaiting) already runs it.
        announcing_ = true;
        CallGlobalInternal(it->second, true);
        announcing_ = false;
    }
}

LuaEngine::WaitState LuaEngine::SuspendWait() {
    WaitState state{waiting_, timed_wait_, wait_accept_input_, click_wait_announced_,
                    se_wait_, transition_wait_, wait_se_key_, video_wait_, wait_until_, auto_timer_.Elapsed(NowMs())};
    waiting_ = timed_wait_ = se_wait_ = transition_wait_ = click_wait_announced_ = false;
    video_wait_.clear();
    auto_timer_.Reset();
    return state;
}

void LuaEngine::RestoreWait(const WaitState& state) {
    waiting_ = state.waiting;
    timed_wait_ = state.timed;
    wait_accept_input_ = state.accept_input;
    click_wait_announced_ = state.announced;
    se_wait_ = state.sound;
    transition_wait_ = state.transition;
    wait_se_key_ = state.sound_key;
    video_wait_ = state.video_key;
    wait_until_ = state.deadline;
    auto_timer_.Restore(NowMs(), state.auto_elapsed);
}

// [wt]/[wait] style time waits: hold the runner until the deadline passes.
void LuaEngine::SetTimedWait(int ms, bool accept_input) {
    wait_until_ = ClockNow() + std::chrono::milliseconds(ms);
    timed_wait_ = true;
    wait_accept_input_ = accept_input;
    SetWaiting(true);
}

// Polled once per frame: auto-clears an expired timed wait so the runner
// resumes without user input. Called by the frame loop before stepping.
bool LuaEngine::IsWaiting() {
    if(!video_wait_.empty()) {
        if(videos_.count(video_wait_)) return true;
        // SetWaiting(false) announces onClickWaitOut first; video_wait_ must
        // still name the movie at that moment or keyClickEnd sees no reason
        // and leaves a stale flg.waitflag.
        SetWaiting(false);
        video_wait_.clear();
    }
    if (timed_wait_ && ClockNow() >= wait_until_) {
        // SetWaiting(false) announces OUT (reason {time} still readable),
        // then clears timed_wait_ itself.
        SetWaiting(false);
    }
    if (se_wait_ && (!sounds_ || !sounds_->IsPlaying(wait_se_key_))) {
        // Same ordering: announce OUT with the {sound} reason intact.
        SetWaiting(false);
    }
    if (waiting_ && !timed_wait_ && !se_wait_ && auto_enabled_) {
        bool blocked = compositor_ && compositor_->PendingTextMs(NowMs()) > 0;
        for (const auto& key : auto_sync_se_)
            if (sounds_ && sounds_->IsPlaying(key)) { blocked = true; break; }
        const auto delay = vars_.find("s.automodewait");
        const double ms = delay == vars_.end() ? 1000 : std::atof(delay->second.c_str());
        if (auto_timer_.Ready(NowMs(), ms, blocked)) SetWaiting(false);
    }
    return waiting_;
}

void LuaEngine::UpdateVideos() {
    for(auto it=videos_.begin();it!=videos_.end();) {
        it->second->Update(NowMs());
        if(!it->second->Active()) it=videos_.erase(it); else ++it;
    }
}

// E-mote layers never auto-expire (the model's base motion keeps animating);
// tick each player, rendering through the compositor when one is attached.
void LuaEngine::UpdateEmotes() {
    for (auto &entry : emotes_) {
        if (entry.second->Update(NowMs(), compositor_, entry.first)) continue;
        Log(kLogError, "E-mote layer failed to render: " + entry.first);
    }
}

// KrKr2-Next: setonsoundfinish callbacks — the framework registers
// `sesys_voiceend` etc. after seplay; fire each once its voice has ended.
void LuaEngine::PollSoundFinish() {
    if (onsoundfinish_.empty()) return;
    std::vector<std::pair<std::string, std::string>> due;
    for (const auto &kv : onsoundfinish_) {
        if (!sounds_ || !sounds_->IsPlaying(kv.first)) due.push_back(kv);
    }
    for (const auto &kv : due) {
        onsoundfinish_.erase(kv.first);
        CallEvent(kv.second, {{"id", kv.first}}, true);
    }
}

// System banks retain the early compatibility encoding for migration. A whole
// bank validates before commit, and atomic replacement preserves the old file
// if a write is interrupted.
void LuaEngine::SetSaveDir(const std::string &dir) {
    save_dir_ = dir;
    sysvals_["savepath"] = dir;
    if (compositor_) compositor_->SetSaveDirectory(dir);
}
bool LuaEngine::SaveSystemData() {
    VariableBank items;for(const auto& kv:vars_)if(kv.first.rfind("t.",0)!=0)items.insert(kv);
    std::vector<uint8_t> bytes;const auto path=SavePath(save_dir_,"system.dat");
    if(!EncodeVariableBank(items,false,bytes) || !WriteSaveFile(path,bytes)) {
        Log(kLogError,"save: cannot write system bank "+path);return false;
    }
    return true;
}
void LuaEngine::LoadSystemData() {
    VariableBank next;std::vector<uint8_t> bytes;const auto path=SavePath(save_dir_,"system.dat");
    std::string error,source=path;
    if(ReadSaveFile(path,bytes)) {
        if(!DecodeVariableBank(bytes,false,next)) {Log(kLogError,"save: invalid system bank "+path);return;}
    } else {
        // Existing compatibility banks are authoritative, including cleared
        // slots/settings. Never merge an old BOWG back over newer progress.
        std::error_code ec;
        if(std::filesystem::exists(path,ec) || ec) {Log(kLogError,"save: cannot read system bank "+path);return;}
        source=SavePath(save_dir_,"saveg.dat");
        if(!ReadSaveFile(source,bytes))return;
        NativeGlobals globals;
        if(!DecodeNativeGlobals(bytes,globals,error)) {Log(kLogError,"save: invalid native globals: "+error);return;}
        next=std::move(globals.variables);
        // Read-line sets are decoded but not applied until native read/skip
        // tracking is implemented. The original BOWG remains untouched.
    }
    if(!ValidateBankGraphs(L_,next,error)) {Log(kLogError,"save: invalid global graph: "+error);return;}
    const bool repaired=RepairCompatibilitySaveDates(L_,next,save_dir_);
    for(auto& kv:next)vars_[kv.first]=std::move(kv.second);
    if(repaired) {Log(kLogInfo,"save: recovered empty compatibility-slot dates from file times");SaveSystemData();}
    Log(kLogInfo,"save: restored variables from "+source);
}
bool LuaEngine::SaveSnapshot(const std::string& file) {
    const auto path=SavePath(save_dir_,file);
    const auto handler=event_handlers_.find("onSave");
    if(path.empty() || path==SavePath(save_dir_,"system.dat") || path==SavePath(save_dir_,"saveg.dat") ||
       saving_ || handler==event_handlers_.end() || handler->second.empty()) {
        Log(kLogError,"save: checkpoint needs a valid slot and onSave callback");return false;
    }
    saving_=true;const bool ready=CallEvent(handler->second,{{"file",file}},false);saving_=false;
    if(!ready)return false;
    VariableBank items;
    for(const auto& v:vars_) {
        const auto prefix=v.first.substr(0,2);
        if(prefix!="g." && prefix!="s." && prefix!="t.")items.insert(v);
    }
    std::vector<uint8_t> bytes;
    if(!EncodeVariableBank(items,true,bytes) || !WriteSaveFile(path,bytes)) {
        Log(kLogError,"save: checkpoint write failed "+file);return false;
    }
    const bool system=SaveSystemData();
    Log(system?kLogInfo:kLogError,"save: checkpoint "+file+(system?" committed":" saved; system bank failed"));
    return system;
}

bool LuaEngine::LoadSnapshot(const std::string& file) {
    auto handler=event_handlers_.find("onLoad");
    if(handler==event_handlers_.end() || handler->second.empty()) {
        Log(kLogError,"load: native snapshot requires the game's onLoad restorer");return false;
    }
    const auto path=SavePath(save_dir_,file);
    std::vector<uint8_t> bytes;
    if(path.empty() || !ReadSaveFile(path,bytes)) {Log(kLogError,"load: cannot read slot "+file);return false;}
    NativeSave snapshot;std::string error;
    const bool native=bytes.size()>=4 && std::memcmp(bytes.data(),"BOWS",4)==0;
    const bool decoded=native ? DecodeNativeSave(bytes,snapshot,error) : DecodeVariableBank(bytes,true,snapshot.variables);
    if(!decoded) {Log(kLogError,"load: invalid snapshot "+file+": "+error);return false;}
    // Validate native Pluto blobs before touching the running state. Raw scalar
    // variables remain strings; closures/VM pointers fail explicitly.
    if(!ValidateBankGraphs(L_,snapshot.variables,error)) {Log(kLogError,"load: "+error);return false;}
    for(const auto& c:snapshot.layers)
        if(c.name!="lyc" && c.name!="lyprop" && c.name!="lyevent" && c.name!="lytween" && c.name!="anime") {
            Log(kLogError,"load: unsupported saved layer command "+c.name);return false;
        }
    // Global/system banks belong to this installation, not to a scenario slot.
    LoadPhase phase=LoadPhase::None;
    auto advance=[&](LoadPhase next){ if(!ValidLoadTransition(phase,next)) Log(kLogWarn,std::string("load: phase order violated -> ")+LoadPhaseName(next)); phase=next; };
    advance(LoadPhase::ResetEphemeral);
    for(auto it=vars_.begin();it!=vars_.end();) {
        const auto prefix=it->first.substr(0,2);
        if(prefix!="g." && prefix!="s.")it=vars_.erase(it);else ++it;
    }
    for(auto& v:snapshot.variables) {
        const auto prefix=v.first.substr(0,2);
        if(prefix!="g." && prefix!="s." && prefix!="t.")vars_[v.first]=std::move(v.second);
    }
    advance(LoadPhase::RestoreData);
    tag_queue_.clear();SuspendWait();SetAutoMode(false);
    save_image_={};
    videos_.clear();emotes_.clear();audio_->StopAll();delete sounds_;sounds_=new AudioChannels(*audio_);
    onsoundfinish_.clear();pending_click_=false;drag_id_.clear();lyevents_.clear();
    if(script_runner_)script_runner_->DiscardFlow();
    advance(LoadPhase::SnapshotScene);
    if(compositor_) {
        const int w=compositor_->StageWidth(),h=compositor_->StageHeight();
        compositor_->ReleaseGl();compositor_->Init(w,h);
        for(const auto& c:snapshot.layers)
            DispatchTag(c.name,{c.attrs.begin(),c.attrs.end()});
    }
    // The registered framework callback reconstructs message pages, audio and
    // the scenario cursor from its restored scr/log/btn graph (quickjump). [B]
    // must run after [A] above or the replay would clobber it.
    advance(LoadPhase::RebuildDerived);
    if(!CallEvent(handler->second,{{"file",file}},false)) return false;
    Log(kLogInfo,std::string("load: ")+(native?"native snapshot":"checkpoint")+" restored via onLoad: "+file+
        " layers="+std::to_string(snapshot.layers.size()));
    return true;
}

// e:getScriptWaitReason() — table whose keys name active non-click waits.
// Official wait reasons are time/textTween/textClearTween/sound/video; values
// are deadlines on the e:now() timeline: keyevent.lua's event_setonpush does
// `w[2] - w[3] <= 0` (remaining ms at wait start) to decide whether a click
// may skip the wait, so a boolean value would raise a Lua arithmetic error.
// A plain click wait is signalled by onClickWaitIn/Out and leaves this table
// empty, which matches the adv framework's getWaitStatus() gate.
int LuaEngine::l_getScriptWaitReason(lua_State *L) {
    auto* self = Self(L);
    // Lazy expiry poll — skipped inside an onClickWaitIn/Out announce, where
    // the flags are mid-transition and the caller must still see the reason
    // (see AnnounceWaitState).
    if (!self->announcing_) self->IsWaiting();
    const double now = self->NowMs();
    lua_newtable(L);
    // sound: delay-guarded in event_setonpush (value unused); video: skip is
    // decided engine-side from video_skip_, so report 0 remaining.
    auto field = [&](const char *name, double deadline) {
        lua_pushnumber(L, deadline);
        lua_setfield(L, -2, name);
    };
    if (!self->video_wait_.empty()) field("video", now);
    if (self->compositor_ && self->compositor_->PendingTextMs(now) > 0)
        field("textTween", now + self->compositor_->PendingTextMs(now));
    if (self->timed_wait_) {
        const double remain = std::chrono::duration<double, std::milli>(
            self->wait_until_ - self->ClockNow()).count();
        field("time", now + (remain > 0 ? remain : 0));
    }
    if (self->se_wait_) field("sound", now);
    return 1;
}

// e:random() — uniform [0,1) (official: 无参数 → number). Used by the media
// framework (sysvo voice pick) expecting a float for % arithmetic.
int LuaEngine::l_random(lua_State *L) {
    // KrKr2-Next: the adv framework uses `e:random() % n + 1` everywhere
    // (sysvo.lua, config.lua, image.lua …) — a C-rand()-style non-negative
    // integer. Returning a [0,1) float made `%` yield a fractional index, so
    // `t[ch]` in sysvo.lua was nil and START on the title screen aborted with
    // "attempt to index field '?'".
    lua_pushinteger(L, static_cast<lua_Integer>(rand()));
    return 1;
}

// e:getScriptStatus() — 0..14 running/waiting state the framework polls.
int LuaEngine::l_getScriptStatus(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushinteger(L, self ? self->script_status_ : 0);
    return 1;
}

int LuaEngine::l_setScriptStatus(lua_State *L) {
    LuaEngine *self = Self(L);
    if (self) self->script_status_ = static_cast<int>(luaL_checkinteger(L, 2));
    return 0;
}

int LuaEngine::l_getScriptSize(lua_State *L) {
    LuaEngine *self = Self(L);
    const size_t size = self && self->script_runner_ ? self->script_runner_->Size() : 0;
    lua_pushinteger(L, static_cast<lua_Integer>(size));
    return 1;
}

int LuaEngine::l_getFrameNumber(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushinteger(L, static_cast<lua_Integer>(self ? self->frame_number_ : 0));
    return 1;
}

int LuaEngine::l_getTouchPoint(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_pushnumber(L, self ? self->mouse_x_ : 0);
    lua_pushnumber(L, self ? self->mouse_y_ : 0);
    return 2;
}

int LuaEngine::l_setFlickSensitivity(lua_State *L) {
    LuaEngine *self = Self(L);
    if (self && lua_isnumber(L, 2))
        self->flick_sensitivity_ = static_cast<float>(lua_tonumber(L, 2));
    return 0;
}

// e:getScriptBlock() — describes the running script block. The framework uses
// it for bookkeeping; expose the current file (empty when no runner).
int LuaEngine::l_getScriptBlock(lua_State *L) {
    LuaEngine *self = Self(L);
    lua_newtable(L);
    if (self && self->script_runner_) {
        const std::string &file = self->script_runner_->CurrentFile();
        lua_pushlstring(L, file.data(), file.size());
        lua_setfield(L, -2, "file");
    }
    return 1;
}

int LuaEngine::l_getScriptStack(lua_State *L) {
    lua_newtable(L);
    LuaEngine* self = Self(L);
    if (self && self->script_runner_) {
        int i = 1;
        for (const auto& file : self->script_runner_->StackFiles()) {
            lua_newtable(L);
            lua_pushlstring(L, file.data(), file.size());
            lua_setfield(L, -2, "file");
            lua_rawseti(L, -2, i++);
        }
    }
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
double LuaEngine::NowMs() const {
    return std::chrono::duration<double, std::milli>(
               ClockNow() - init_time_).count();
}

int LuaEngine::l_now(lua_State *L) {
    LuaEngine *self = Self(L);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        self->ClockNow() - self->init_time_).count();
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
    if (PCallTraceback(L_, 0, 0) != 0) {
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
    if (!PushGlobalFn(fn, quiet)) return false;
    lua_getglobal(L_, kBridgeTable);   // engine bridge passed as arg 1
    if (PCallTraceback(L_, 1, 0) != 0) {
        if (!quiet)
            Log(kLogError, "calllua " + fn + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

// Route an engine tag through the e:tag bridge (tag name = array item 1).
int LuaEngine::FilterTag(const std::string &tag,
                         const std::vector<std::pair<std::string, std::string>> &attrs,
                         std::string *replacement) {
    if (tag_filter_ref_ < 0) return 0;
    lua_rawgeti(L_, LUA_REGISTRYINDEX, tag_filter_ref_);
    int nargs = 0;
    if (lua_isfunction(L_, -1)) {
        lua_getglobal(L_, kBridgeTable);
        lua_pushlstring(L_, tag.data(), tag.size());
        lua_newtable(L_);
        for (const auto &kv : attrs) {
            lua_pushlstring(L_, kv.second.data(), kv.second.size());
            lua_setfield(L_, -2, kv.first.c_str());
        }
        nargs = 3;
    } else if (lua_istable(L_, -1)) {
        lua_getfield(L_, -1, tag.c_str());
        if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); return 0; }
        lua_getglobal(L_, kBridgeTable);
        lua_newtable(L_);
        for (const auto &kv : attrs) {
            lua_pushlstring(L_, kv.second.data(), kv.second.size());
            lua_setfield(L_, -2, kv.first.c_str());
        }
        lua_remove(L_, -4); // drop the filter table below fn/bridge/attrs
        nargs = 2;
    } else {
        lua_pop(L_, 1);
        return 0;
    }
    if (PCallTraceback(L_, nargs, 1) != 0) {
        Log(kLogError, "tag filter " + tag + ": " + std::string(lua_tostring(L_, -1)));
        lua_pop(L_, 1);
        return 0; // allow on error
    }
    int result = 0;
    if (lua_isboolean(L_, -1)) result = lua_toboolean(L_, -1) ? 1 : 0;
    else if (lua_isnumber(L_, -1)) result = lua_tointeger(L_, -1) != 0 ? 1 : 0;
    else if (lua_isstring(L_, -1)) {
        if (replacement) *replacement = lua_tostring(L_, -1);
        result = 2;
    }
    lua_pop(L_, 1);
    return result;
}

int LuaEngine::l_setTagFilter(lua_State *L) {
    auto *self = Self(L);
    if (!lua_isnoneornil(L, 2) && !lua_isfunction(L, 2) && !lua_istable(L, 2))
        return luaL_error(L, "setTagFilter expects a function, table or nil");
    luaL_unref(L, LUA_REGISTRYINDEX, self->tag_filter_ref_);
    if (lua_isnoneornil(L, 2)) lua_pushnil(L);
    else lua_pushvalue(L, 2);
    self->tag_filter_ref_ = luaL_ref(L, LUA_REGISTRYINDEX);
    return 0;
}

bool LuaEngine::DispatchTag(const std::string &tag,
                            const std::vector<std::pair<std::string, std::string>> &attrs,
                            bool apply_filter) {
    if (!L_) return false;
    std::string replacement;
    if (apply_filter) {
        const int filtered = FilterTag(tag, attrs, &replacement);
        if (filtered == 1) return true; // intercepted
    }
    const std::string &effective = (apply_filter && !replacement.empty()) ? replacement : tag;
    lua_getglobal(L_, kBridgeTable);
    if (!lua_istable(L_, -1)) { lua_pop(L_, 1); return false; }
    lua_getfield(L_, -1, "tag");
    if (!lua_isfunction(L_, -1)) { lua_pop(L_, 2); return false; }
    lua_pushvalue(L_, -2);             // self
    lua_newtable(L_);                  // tag table
    lua_pushlstring(L_, effective.c_str(), effective.size());
    lua_rawseti(L_, -2, 1);
    for (const auto &kv : attrs) {
        lua_pushlstring(L_, kv.first.c_str(), kv.first.size());
        lua_pushlstring(L_, kv.second.c_str(), kv.second.size());
        lua_settable(L_, -3);
    }
    if (PCallTraceback(L_, 2, 0) != 0) {
        Log(kLogError, "tag dispatch " + tag + ": " + lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    lua_pop(L_, 1);                    // pop the e table
    return true;
}

} // namespace artc
