// lua_tags_input.cpp — input, auto-mode and the seton*/delon* event
// registries.
//
// The framework's key-callback machinery lives here: setonpush routes button
// activation / dialog / UI key handling when a key is pressed, keyconfig
// maps key ids to roles, and the generic setonX/delonX registry stores the
// handler kinds without dedicated engine state (backlog / commandskip /
// controlskip / dirchg / hide / windowbutton ...).
#include "script/lua_engine.h"
#include "log/logger.h"
#include <cstdlib>
#include <sstream>
#include <string>

namespace artc {

bool LuaEngine::TagAutoMode(const std::string &tag, TagAttrs &m) {
    if (m.count("allow")) {
        auto_allowed_ = m["allow"] != "0";
        if (!auto_allowed_) SetAutoMode(false);
    }
    if (m.count("stopbyclick")) auto_stop_click_ = m["stopbyclick"] != "0";
    if (m.count("stopbystop")) auto_stop_stop_ = m["stopbystop"] != "0";
    if (m.count("syncse")) {
        auto_sync_se_.clear();
        std::istringstream list(m["syncse"]);
        std::string key;
        while (std::getline(list, key, ',')) if (!key.empty()) auto_sync_se_.push_back(key);
        auto_timer_.Reset();
    }
    return true;
}

bool LuaEngine::TagExecAutoMode(const std::string &tag, TagAttrs &m) {
    if (m["command"] != "automode") return false;
    SetAutoMode(m.count("mode") ? m["mode"] != "0" : !auto_enabled_);
    return true;
}

bool LuaEngine::TagAutoModeEvent(const std::string &tag, TagAttrs &m) {
    const auto event = tag.substr(3);
    if (tag.compare(0, 3, "set") == 0) auto_events_[event] = {m.begin(), m.end()};
    else auto_events_.erase(event);
    return true;
}

bool LuaEngine::TagKeyConfig(const std::string &tag, TagAttrs &m) {
    auto& keys=key_roles_[std::atoi(m["role"].c_str())];
    keys.clear();
    const auto& list=m["keys"];
    size_t start=0;
    while(start<list.size()) {
        const size_t end=list.find(',',start);
        const auto item=list.substr(start,end==std::string::npos ? end : end-start);
        char* tail=nullptr;
        const long key=std::strtol(item.c_str(),&tail,10);
        if(tail!=item.c_str() && *tail=='\0' && key>=0 && key<InputState::Count)
            keys.insert(static_cast<int>(key));
        if(end==std::string::npos) break;
        start=end+1;
    }
    return true;
}

// "@" / "p" / "clickwait" — pause the native runner until the next tap.
bool LuaEngine::TagClickWait(const std::string &tag, TagAttrs &m) {
    SetWaiting(true);
    return true;
}

// calllua (framework call_lua()): button exec / p4 callbacks arrive as an
// e:tag {"calllua", function="fn", ...} — e.g. the language buttons'
// exec="langsel_click". The framework convention is fn(e, tag_attrs): the
// attrs table (key/name/btn/...) is param 2 — langsel_click reads p.btn.
// (The asb runner's native calllua lines take the function-only path and
// don't pass through here.)
bool LuaEngine::TagCallLua(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    if (!m.count("function") || m["function"].empty()) return false;
    if (m["function"] == "fn.pop")
        DoString("local fs=flg and flg.funcstack\n"
             "local c=fn and fn.name and fs and fs[fn.name]\n"
             "e:debug('POP stack='..tostring(fn and fn.name)"
             "..' n='..tostring(c and #c)"
             "..' param='..tostring(fn and fn.param))", "pop");
    std::vector<std::pair<std::string, std::string>> params(m.begin(), m.end());
    CallEvent(m["function"], params, false);
    return true;
}

// KrKr2-Next: setonsoundfinish {id, function} — fired once the voice ends.
bool LuaEngine::TagSoundFinish(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const std::string sid = m["id"];
    if (!sid.empty()) {
        if (tag == "setonsoundfinish" && !m["function"].empty())
            onsoundfinish_[sid] = m["function"];
        else
            onsoundfinish_.erase(sid);
    }
    return true;
}

// Framework key-callback registry: setonpush{key=N, function, adv, ui,
// btn} — when key N is pressed the engine calls setonpush_calllua,
// which routes button activation / dialog / UI key handling. Without
// this, tap-driven UI (dialog yes/no, key nav) stays dead.
bool LuaEngine::TagSetOnPush(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const auto &ks = m.find("key");
    const int key = ks == m.end() ? 0 : std::atoi(ks->second.c_str());
    if (key > 0) {
        if (tag == "setonpush") {
            onpush_[key] = {m.begin(), m.end()};
        } else {
            onpush_.erase(key);
        }
    }
    return true;
}

// Generic setonX/delonX registry (handler kinds without dedicated state).
// Runs after the exact-name table so dedicated handlers (push /
// soundfinish / automodein/out) keep their own paths; videofinish is
// intentionally unhandled (no engine hook for it yet).
bool LuaEngine::TagGenericSetOn(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    if (!(tag.rfind("seton", 0) == 0 || tag.rfind("delon", 0) == 0)) return false;
    const std::string key = tag.substr(3); // drop the set/del prefix
    if (key != "push" && key != "soundfinish" && key != "videofinish" &&
        key != "automodein" && key != "automodeout") {
        if (tag.compare(0, 3, "set") == 0)
            named_events_[key] = {m.begin(), m.end()};
        else
            named_events_.erase(key);
    }
    return true;
}

} // namespace artc
