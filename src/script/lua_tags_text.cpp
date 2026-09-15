// lua_tags_text.cpp — message-layer text pipeline.
//
// Framework (msg/message.lua): chgmsg_adv selects the message text layer
// via e:tag{"chgmsg", id=...}; mw_textloop then emits print{data} rows
// (speaker name / line text) into that layer, rt ends a line, /chgmsg
// closes the selection. Official print rasterizes `data` into the selected
// layer (engine-side text). We accumulate the page, retaining explicit line
// breaks and layer style. The `font` tag records its height on the raw
// stage (TagFontHeightRaw) and runs the full restyle here.
#include "script/lua_engine.h"
#include "render/compositor.h"
#include "script/dialog_request.h"
#include "log/logger.h"
#include <cctype>
#include <cstdlib>
#include <string>

namespace artc {

bool LuaEngine::TagFontHeightRaw(lua_State *L) {
    // Message-layer font parameters; the height feeds the engine query
    // get_message_layer_height (see TagVarRaw). Height only — the full
    // `font` handling still runs through the common path (TagFont).
    lua_getfield(L, 2, "height");
    const int h = static_cast<int>(lua_tointeger(L, -1));
    if (h > 0) msg_layer_height_ = h;
    lua_pop(L, 1);
    return false;
}

bool LuaEngine::TagProhibitRaw(lua_State *L) {
    lua_getfield(L, 2, "head");
    const char *head = lua_tostring(L, -1);
    lua_getfield(L, 2, "foot");
    const char *foot = lua_tostring(L, -1);
    if (compositor_)
        compositor_->SetProhibitRules(head ? head : "", foot ? foot : "");
    lua_pop(L, 2);
    return true;
}

bool LuaEngine::TagWordpartsRaw(lua_State *L) {
    lua_getfield(L, 2, "parts");
    const char *parts = lua_tostring(L, -1);
    if (compositor_) compositor_->SetWordparts(parts ? parts : "");
    lua_pop(L, 1);
    return true;
}

bool LuaEngine::TagIndentRaw(lua_State *L) {
    lua_getfield(L, 2, "pair");
    const char *pair = lua_tostring(L, -1);
    lua_getfield(L, 2, "range");
    const int range = lua_isnil(L, -1) ? -1 : static_cast<int>(lua_tointeger(L, -1));
    lua_getfield(L, 2, "nest");
    const bool nest = lua_tointeger(L, -1) != 0;
    if (compositor_)
        compositor_->SetIndentRules(pair ? pair : "", range, nest);
    lua_pop(L, 3);
    return true;
}

bool LuaEngine::TagFont(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    // [fontdefault] supplies fallbacks for any attribute the tag omits.
    std::map<std::string, std::string> merged = font_defaults_;
    for (const auto &kv : m) merged[kv.first] = kv.second;
    // Load the face once. The tag restyles the CURRENT chgmsg layer;
    // fonts with show=none belong to hidden/off-screen slots (e.g.
    // top=-5 measure slots) and must never become the visible layout.
    if (!font_loaded_) {
        auto face = merged.find("face");
        if (face != merged.end()) {
            compositor_->SetPackManager(packs_);
            if (compositor_->LoadFont(
                    ResolvePackPath(face->second)))
                font_loaded_ = true;
        }
    }
    if (!msg_layer_.empty())
        for (const auto& kv : merged) font_of_[msg_layer_][kv.first] = kv.second;
    auto hidden = merged.find("show");
    if (hidden != merged.end() && hidden->second == "none") return true;
    auto &slot = [&]() -> std::map<std::string, std::string> & {
        auto w = merged.find("width");
        return (w != merged.end() && std::atof(w->second.c_str()) >= 700)
                   ? font_main_
                   : font_name_;
    }();
    slot = merged;
    return true;
}

bool LuaEngine::TagFontDefault(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    font_defaults_ = {m.begin(), m.end()};
    return true;
}

bool LuaEngine::TagFontInit(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    // Reset the current font state only. Per-layer rects registered by
    // earlier chgmsg+font pairs must survive, or every set_textfont
    // call would wipe the geometry of the layers before it.
    font_defaults_.clear();
    font_main_.clear();
    font_name_.clear();
    if (!msg_layer_.empty()) font_of_.erase(msg_layer_);
    return true;
}

bool LuaEngine::TagFontClose(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    if (!msg_layer_.empty()) font_of_.erase(msg_layer_);
    return true;
}

bool LuaEngine::TagGlyph(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    glyph_config_ = {m.begin(), m.end()};
    return true;
}

bool LuaEngine::TagChgMsg(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    auto it = m.find("id");
    msg_layer_ = it == m.end() ? std::string() : it->second;
    return true;
}

bool LuaEngine::TagScetween(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    compositor_->SetTextTween(msg_layer_, m);
    return true;
}

bool LuaEngine::TagChgMsgEnd(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    msg_layer_.clear();
    return true;
}

bool LuaEngine::TagRp(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || msg_layer_.empty()) return false;
    msg_text_.erase(msg_layer_);
    compositor_->SetText(msg_layer_, "", 40, 0xffffff);
    return true;
}

bool LuaEngine::TagRuby(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || msg_layer_.empty()) return false;
    auto& page=msg_text_[msg_layer_];
    if (!page.ruby_active) {
        page.ruby_active=true; page.ruby_start=page.text.size(); page.ruby_text=m["text"];
    }
    return true;
}

bool LuaEngine::TagRubyEnd(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || msg_layer_.empty()) return false;
    auto& page=msg_text_[msg_layer_];
    if (page.ruby_active) {
        page.ruby_active=false;
        page.ruby.push_back({page.ruby_start,page.text.size()-page.ruby_start,page.ruby_text});
        DispatchTag("print",{{"data",""}});
    }
    return true;
}

bool LuaEngine::TagPrint(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || msg_layer_.empty()) return false;
    auto& page = msg_text_[msg_layer_];
    auto& text = page.text;
    text += m["data"];
    if (page.ruby_active) return true;
    // font resolution: the rect registered for THIS layer via
    // chgmsg+font pairing wins; otherwise the last visible-area font.
    auto fo = font_of_.find(msg_layer_);
    const std::map<std::string, std::string> &fr =
        fo != font_of_.end()
            ? fo->second
            : (msg_layer_.find("name") != std::string::npos
                   ? font_name_
                   : font_main_);
    const float size =
        fr.count("size") ? std::atof(fr.at("size").c_str()) : 40.f;
    if (fr.count("face"))
        compositor_->LoadFont(ResolvePackPath(fr.at("face")));
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
        Log(kLogInfo, "print: layer=" + msg_layer_ + " wrap=" +
                          std::to_string((int)wrap) + "(" + wrap_src + ") size=" +
                          std::to_string((int)size));
    }
    compositor_->SetText(
            msg_layer_, text, size, color, wrap, fr, page.ruby);
    return true;
}

bool LuaEngine::TagRt(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || msg_layer_.empty()) return false;
    auto& text = msg_text_[msg_layer_].text;
    if (m["omitblankline"] != "1" || (!text.empty() && text.back() != '\n')) text += '\n';
    return true;
}

bool LuaEngine::TagLink(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    link_active_ = true;
    link_enabled_ = true;
    link_file_ = m.count("file") ? m["file"] : std::string();
    link_label_ = m.count("label") ? m["label"] : std::string();
    return true;
}

bool LuaEngine::TagLinkEnd(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    link_active_ = false;
    return true;
}

bool LuaEngine::TagLinkDisable(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    link_enabled_ = false;
    return true;
}

bool LuaEngine::TagLinkEnable(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    link_enabled_ = true;
    return true;
}

// Native host dialog (framework tag_dialog): message-only alert, yes/no
// confirm (varname), or text input (textfield). The host fills
// text/accepted; we write the result variables back.
bool LuaEngine::TagDialog(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    DialogRequest req;
    req.title = m.count("title") ? m["title"] : std::string();
    req.message = m.count("message") ? m["message"] : std::string();
    req.textfield = m.count("textfield") ? m["textfield"] : std::string();
    req.textfieldsize = m.count("textfieldsize")
                            ? std::atoi(m["textfieldsize"].c_str()) : 0;
    req.has_input = !req.textfield.empty();
    const std::string varname = m.count("varname") ? m["varname"] : std::string();
    req.has_result = !varname.empty();
    if (dialog_handler_) dialog_handler_(req);
    else Log(kLogWarn, "dialog: no host handler; treating as dismissed");
    if (!req.textfield.empty()) vars_[req.textfield] = req.text;
    if (!varname.empty()) vars_[varname] = req.accepted ? "1" : "0";
    Log(kLogInfo, "dialog: kind=" +
                      std::string(req.has_input ? "input"
                                                : (req.has_result ? "confirm" : "alert")) +
                      " accepted=" + (req.accepted ? "1" : "0") +
                      " len=" + std::to_string(req.text.size()));
    return true;
}

} // namespace artc
