// lua_tag_dispatch.cpp — e:tag{...} entry point and name→handler tables.
//
// l_tag is a thin two-stage dispatcher:
//   1. raw handlers read attrs straight off the Lua table and mostly early-
//      return (TagFontHeightRaw deliberately falls through so the common
//      path still runs — the `font` tag records its height first, then the
//      full handler sees the attr map);
//   2. after the shared attr map `m` is built (values ResolveValue'd, see
//      TraceAndCollectAttrs), an exact-name table routes to the domain
//      handler, and the generic seton*/delon* prefix registry catches the
//      handler kinds without dedicated state.
// Handler return values mirror the original if-chain fall-through: false =
// a gate (compositor_ / sounds_ / …) was not met, so nothing happened and
// the tag ends as a no-op — exactly what the old chain did by skipping the
// block.
#include "script/lua_engine.h"
#include "log/logger.h"
#include <map>
#include <string>

namespace artc {

// Common path: debug log, first-occurrence tag trace, and the attr map.
// Only string keys are tag attributes (array key 1 = tag name); lua_tostring
// would convert a NUMBER key in place and corrupt lua_next iteration
// ("invalid key to 'next'"), so guard by type. Used by the wait gate and the
// compositor.
LuaEngine::TagAttrs LuaEngine::TraceAndCollectAttrs(lua_State *L,
                                                     const std::string &tagname) {
    Log(kLogDebug, "tag: " + tagname);
    // first-occurrence tag trace: reveals what the scripts ask the engine
    // for without flooding logcat at frame rate
    {
        std::string summary = tagname;
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
        if (seen_tags_.insert(summary).second) Log(kLogInfo, "tag[trace]: " + summary);
    }
    TagAttrs m;
    lua_pushnil(L);
    while (lua_next(L, 2) != 0) {
        if (lua_type(L, -2) == LUA_TSTRING) {
            const char *k = lua_tostring(L, -2);
            const char *v = lua_tostring(L, -1); // numbers: value slot, safe
            if (k && v) m[k] = ResolveValue(v);
        }
        lua_pop(L, 1);
    }
    return m;
}

const std::unordered_map<std::string, LuaEngine::TagRawHandler> &
LuaEngine::RawTagTable() {
    static const std::unordered_map<std::string, LuaEngine::TagRawHandler> k = {
        {"var", &LuaEngine::TagVarRaw},
        // Records the message-layer height, then falls through (return false)
        // so the full TagFont handler still sees the attr map.
        {"font", &LuaEngine::TagFontHeightRaw},
        {"prohibit", &LuaEngine::TagProhibitRaw},
        {"wordparts", &LuaEngine::TagWordpartsRaw},
        {"indent", &LuaEngine::TagIndentRaw},
        {"lyrename", &LuaEngine::TagLyrenameRaw},
        {"allsoundstop", &LuaEngine::TagAllSoundStopRaw},
        {"debug", &LuaEngine::TagDebugRaw},
    };
    return k;
}

const std::unordered_map<std::string, LuaEngine::TagHandler> &
LuaEngine::TagTable() {
    static const std::unordered_map<std::string, LuaEngine::TagHandler> k = {
        // input / auto-mode
        {"automode", &LuaEngine::TagAutoMode},
        {"exec", &LuaEngine::TagExecAutoMode},
        {"setonautomodein", &LuaEngine::TagAutoModeEvent},
        {"setonautomodeout", &LuaEngine::TagAutoModeEvent},
        {"delonautomodein", &LuaEngine::TagAutoModeEvent},
        {"delonautomodeout", &LuaEngine::TagAutoModeEvent},
        {"keyconfig", &LuaEngine::TagKeyConfig},
        // Click-wait gate ("@" is the pre-@ spelling). "rp" is deliberately
        // NOT here: the official tag docs define it as a scenario page split
        // (message layer) and the framework dispatches it through e:tag
        // during normal page display — treating it as a wait deadlocks the
        // runner at every paragraph.
        {"@", &LuaEngine::TagClickWait},
        {"p", &LuaEngine::TagClickWait},
        {"clickwait", &LuaEngine::TagClickWait},
        {"calllua", &LuaEngine::TagCallLua},
        {"setonpush", &LuaEngine::TagSetOnPush},
        {"delonpush", &LuaEngine::TagSetOnPush},
        {"setonsoundfinish", &LuaEngine::TagSoundFinish},
        {"delonsoundfinish", &LuaEngine::TagSoundFinish},
        // navigation / wait / system save
        {"video", &LuaEngine::TagVideo},
        {"wait", &LuaEngine::TagWait},
        {"stop", &LuaEngine::TagStopReturn},
        {"return", &LuaEngine::TagStopReturn},
        {"jump", &LuaEngine::TagJump},
        {"call", &LuaEngine::TagCall},
        {"reset", &LuaEngine::TagReset},
        {"exit", &LuaEngine::TagExit},
        {"save", &LuaEngine::TagSave},
        {"load", &LuaEngine::TagLoad},
        {"takess", &LuaEngine::TagTakeSs},
        {"savess", &LuaEngine::TagSaveSs},
        {"wt", &LuaEngine::TagWt},
        // audio: one handler computes the shared channel/time/gain params
        {"splay", &LuaEngine::TagAudio},        {"sxfade", &LuaEngine::TagAudio},
        {"sstop", &LuaEngine::TagAudio},         {"sfade", &LuaEngine::TagAudio},
        {"span", &LuaEngine::TagAudio},          {"seplay", &LuaEngine::TagAudio},
        {"voplay", &LuaEngine::TagAudio},        {"vbplay", &LuaEngine::TagAudio},
        {"bplay", &LuaEngine::TagAudio},         {"s2play", &LuaEngine::TagAudio},
        {"sestop", &LuaEngine::TagAudio},        {"sefade", &LuaEngine::TagAudio},
        {"sepan", &LuaEngine::TagAudio},
        // layers / transitions
        {"lyshader", &LuaEngine::TagLyShader},
        {"lyevent", &LuaEngine::TagLyEvent},
        {"lyc", &LuaEngine::TagLyCreate},
        {"lyprop", &LuaEngine::TagLyProp},
        {"tweenset", &LuaEngine::TagTweenSet},
        {"/tweenset", &LuaEngine::TagTweenSetEnd},
        {"lytween", &LuaEngine::TagLyTween},
        {"lytweendel", &LuaEngine::TagLyTweenDel},
        {"lydel", &LuaEngine::TagLyDel},
        {"flip", &LuaEngine::TagFlip},
        {"trans", &LuaEngine::TagTrans},
        {"lyedit", &LuaEngine::TagLyEdit},
        {"anime", &LuaEngine::TagAnime},
        {"uitrans", &LuaEngine::TagUiTrans},
        {"lydrag", &LuaEngine::TagLyDrag},
        {"hide", &LuaEngine::TagHide},
        // Recognized engine-informational / config tags: the framework owns
        // their UI, so store nothing and let the script continue instead of
        // dispatching them as unknown.
        {"scein", &LuaEngine::TagNoOp},          {"sceout", &LuaEngine::TagNoOp},
        {"backlog", &LuaEngine::TagNoOp},         {"alreadyread", &LuaEngine::TagNoOp},
        {"writebacklog", &LuaEngine::TagNoOp},   {"rclick", &LuaEngine::TagNoOp},
        {"sysshow", &LuaEngine::TagNoOp},         {"syshide", &LuaEngine::TagNoOp},
        {"loadmask", &LuaEngine::TagNoOp},        {"alldelete", &LuaEngine::TagNoOp},
        {"repeatedly", &LuaEngine::TagNoOp},      {"autoskip_disable", &LuaEngine::TagNoOp},
        {"macroadd", &LuaEngine::TagNoOp},        {"macrodel", &LuaEngine::TagNoOp},
        {"loading", &LuaEngine::TagNoOp},         {"saving", &LuaEngine::TagNoOp},
        // message-layer text pipeline
        {"fontdefault", &LuaEngine::TagFontDefault},
        {"fontinit", &LuaEngine::TagFontInit},
        {"font_close", &LuaEngine::TagFontClose},
        {"glyph", &LuaEngine::TagGlyph},
        {"chgmsg", &LuaEngine::TagChgMsg},
        {"scetween", &LuaEngine::TagScetween},
        {"/chgmsg", &LuaEngine::TagChgMsgEnd},
        {"rp", &LuaEngine::TagRp},
        {"ruby", &LuaEngine::TagRuby},
        {"/ruby", &LuaEngine::TagRubyEnd},
        {"print", &LuaEngine::TagPrint},
        {"rt", &LuaEngine::TagRt},
        {"link", &LuaEngine::TagLink},
        {"/link", &LuaEngine::TagLinkEnd},
        {"linkdisable", &LuaEngine::TagLinkDisable},
        {"linkenable", &LuaEngine::TagLinkEnable},
        {"dialog", &LuaEngine::TagDialog},
        // Note: "font" is here too — the raw stage only records the height
        // and falls through, the common path then runs the full handler.
        {"font", &LuaEngine::TagFont},
    };
    return k;
}

// e:tag{ "tagname", key=value, ... } — M0: log + implement `var` and `debug`.
int LuaEngine::l_tag(lua_State *L) {
    LuaEngine *self = Self(L);
    if (!self || !lua_istable(L, 2)) return 0;

    // element 1 of the array part is the tag name
    lua_rawgeti(L, 2, 1);
    const char *tag = lua_tostring(L, -1);
    std::string tagname = tag ? tag : "";
    lua_pop(L, 1);

    // Stage 1: raw-table handlers (attrs read straight off the Lua table).
    const auto raw = RawTagTable().find(tagname);
    if (raw != RawTagTable().end() && (self->*(raw->second))(L)) return 0;

    // Stage 2: common path (trace + attr map), exact-name table, then the
    // generic seton*/delon* prefix registry for kinds without dedicated state.
    TagAttrs m = self->TraceAndCollectAttrs(L, tagname);
    const auto it = TagTable().find(tagname);
    if (it != TagTable().end() && (self->*(it->second))(tagname, m)) return 0;
    self->TagGenericSetOn(tagname, m);
    return 0;
}

} // namespace artc
