// lua_tags_var.cpp — `var` and `debug` tag handlers.
//
// The `var` tag is the engine's variable system: direct assignment
// (name+data), the system= helper family (random / substr / explode /
// base64 / date / layer info / …) and the sysvals passthrough. `debug`
// mirrors the ini DEBUG_MODE / DEBUG_LEVEL globals.
#include "script/lua_engine.h"
#include "render/compositor.h"
#include "log/logger.h"
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>

namespace artc {

// {"var", name="t.os", system="os"} → store the engine system value
bool LuaEngine::TagVarRaw(lua_State *L) {
    lua_getfield(L, 2, "name");
    const char *name = lua_tostring(L, -1);
    // {"var", name=…, data=pluto串} — fsave_pluto bank. The [save] tag
    // persists these; on reboot LoadSystemData re-injects them so the
    // framework's fload_pluto→e:var can restore sys/conf/gscr.
    lua_getfield(L, 2, "data");
    size_t data_size=0;
    const char *data = lua_tolstring(L, -1, &data_size);
    if (name && data) {
        vars_[name] = ResolveValue(std::string(data,data_size));
        lua_pop(L, 2);
        return true;
    }
    lua_pop(L, 1);   // nil data
    lua_getfield(L, 2, "system");
    const char *sys = lua_tostring(L, -1);
    if (name && sys) {
        auto field = [&](const char *k) -> std::string {
            lua_getfield(L, 2, k);
            const char *s = lua_tostring(L, -1);
            std::string r = s ? s : "";
            lua_pop(L, 1);
            return r;
        };
        auto rfield = [&](const char *k) -> std::string {
            return ResolveValue(field(k));
        };
        const std::string S(sys);
        if (S == "delete") {
            if (std::string(name).empty()) vars_.clear();
            else vars_.erase(name);
        } else if (S == "var_exist") {
            const std::string target = field("target");
            const bool exists = vars_.count(target) || sysvals_.count(target);
            vars_[name] = exists ? "1" : "0";
        } else if (S == "random") {
            const long long mn = rfield("min").empty() ? 0 : std::strtoll(rfield("min").c_str(), nullptr, 0);
            const std::string max_s = rfield("max");
            const long long mx = max_s.empty() ? INT64_MAX : std::strtoll(max_s.c_str(), nullptr, 0);
            long long v = mn;
            if (mx > mn) v = mn + (static_cast<long long>(rand()) % (mx - mn + 1));
            vars_[name] = std::to_string(v);
        } else if (S == "length") {
            const std::string src = rfield("source");
            vars_[name] = std::to_string(field("mode") == "1"
                                                   ? Utf8Length(src)
                                                   : static_cast<int>(src.size()));
        } else if (S == "find") {
            const std::string src = rfield("source");
            const std::string needle = rfield("string");
            const size_t pos = src.find(needle);
            vars_[name] = std::to_string(pos == std::string::npos ? -1
                                                                   : static_cast<long long>(pos));
        } else if (S == "substr") {
            const std::string src = rfield("source");
            const long long pos = std::strtoll(rfield("position").c_str(), nullptr, 0);
            const std::string len_s = rfield("length");
            const long long len = len_s.empty() ? static_cast<long long>(src.size())
                                                : std::strtoll(len_s.c_str(), nullptr, 0);
            if (field("mode") == "1") {
                vars_[name] = Utf8Substr(src, pos < 0 ? 0 : static_cast<size_t>(pos),
                                               len < 0 ? 0 : static_cast<size_t>(len));
            } else {
                const size_t start = pos < 0 ? 0 : std::min<size_t>(pos, src.size());
                const size_t end = std::min<size_t>(start + (len < 0 ? 0 : len), src.size());
                vars_[name] = src.substr(start, end - start);
            }
        } else if (S == "explode") {
            const auto parts = SplitEscaped(rfield("source"), field("delimiter"), field("escape"));
            for (size_t i = 0; i < parts.size(); ++i)
                vars_[std::string(name) + "." + std::to_string(i)] = parts[i];
            vars_[std::string(name) + ".size"] = std::to_string(parts.size());
        } else if (S == "unixtime") {
            vars_[name] = std::to_string(static_cast<long long>(std::time(nullptr)));
        } else if (S == "base64_encode") {
            vars_[name] = Base64Encode(rfield("source"));
        } else if (S == "url_encode") {
            vars_[name] = UrlEncode(rfield("source"));
        } else if (S == "url_decode") {
            vars_[name] = UrlDecode(rfield("source"));
        } else if (S == "fullscreen" || S == "minimize") {
            vars_[name] = "0";
        } else if (S == "screen_width") {
            vars_[name] = sysvals_.count("screen_width")
                                ? sysvals_["screen_width"] : "1280";
        } else if (S == "screen_height") {
            vars_[name] = sysvals_.count("screen_height")
                                ? sysvals_["screen_height"] : "720";
        } else if (S == "file_exist" || S == "file_exists") {
            const std::string file = rfield("file");
            bool exists = false;
            if (file.size() >= 4) {
                std::string low = file;
                for (char &c : low) c = static_cast<char>(std::tolower((unsigned char)c));
                exists = low.compare(low.size() - 4, 4, ".exe") == 0;
            }
            if (!exists && packs_) exists = packs_->Exists(file);
            vars_[name] = exists ? "1" : "0";
        } else if (S == "date") {
            // Save metadata reads six dotted calendar fields, not a Unix
            // timestamp or the literal string "date". Use local wall time
            // independently of the pausable monotonic animation clock.
            const std::time_t now=std::time(nullptr);std::tm date{};
#if defined(_WIN32)
            const bool valid=localtime_s(&date,&now)==0;
#else
            const bool valid=localtime_r(&now,&date)!=nullptr;
#endif
            if(valid) {
                const std::string prefix=std::string(name)+".";
                for(const auto& field:std::vector<std::pair<std::string,int>>{
                    {"year",date.tm_year+1900},{"month",date.tm_mon+1},{"day",date.tm_mday},
                    {"hour",date.tm_hour},{"minute",date.tm_min},{"second",date.tm_sec}})
                    vars_[prefix+field.first]=std::to_string(field.second);
            } else Log(kLogError,"date: local wall clock conversion failed");
        } else if (std::string(sys) == "get_message_layer_height") {
            // Framework query (msg/ui.lua uihelp_over): the message
            // layer's content height after font layout — used to center
            // UI help text vertically. We approximate with the height of
            // the most recent `font` tag.
            vars_[name] = std::to_string(msg_layer_height_);
        } else if (std::string(sys) == "get_layer_info") {
            // slider_dragX reads the pinned layer's stored left to derive
            // the drag percentage — return the raw stored offsets.
            lua_getfield(L, 2, "id");
            const char *lid = lua_tostring(L, -1);
            lua_pop(L, 1);
            if (lid && compositor_) {
                const auto info = compositor_->GetLayerInfo(lid);
                if (info.found) {
                    const std::string nm(name);
                    // The framework reads <name>.left/.top/.width/.height
                    // (e.g. getTabletPos: e:var("t.ly.left")). Store the
                    // bare name too for older callers.
                    vars_[nm] = std::to_string((int)info.left);
                    vars_[nm + ".left"] = std::to_string((int)info.left);
                    vars_[nm + ".top"] = std::to_string((int)info.top);
                    vars_[nm + ".width"] = std::to_string((int)info.width);
                    vars_[nm + ".height"] = std::to_string((int)info.height);
                    if (std::string(lid).find("zmask") != std::string::npos ||
                        std::string(lid).find(".mw.tb") != std::string::npos)
                        Log(kLogInfo, std::string("layer_info: ") + lid + " left=" +
                                          std::to_string((int)info.left) + " width=" +
                                          std::to_string((int)info.width));
                }
            }
        } else {
            auto it = sysvals_.find(sys);
            vars_[name] = it == sysvals_.end() ? sys
                                               : it->second;
        }
    }
    lua_pop(L, 2);
    return true;
}

bool LuaEngine::TagDebugRaw(lua_State *L) {
    lua_getfield(L, 2, "mode");
    lua_getfield(L, 2, "level");
    debug_mode_ = static_cast<int>(lua_tointeger(L, -2));
    debug_level_ = static_cast<int>(lua_tointeger(L, -1));
    lua_pushinteger(L, debug_mode_);
    lua_setglobal(L, "DEBUG_MODE");
    lua_pushinteger(L, debug_level_);
    lua_setglobal(L, "DEBUG_LEVEL");
    lua_pop(L, 2);
    Log(kLogInfo, "debug tag: mode=" + std::to_string(debug_mode_) +
                      " level=" + std::to_string(debug_level_));
    return true;
}

} // namespace artc
