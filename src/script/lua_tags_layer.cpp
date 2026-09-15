// lua_tags_layer.cpp — layer lifecycle, transforms, tweens, transitions.
//
// lyc/lyprop/lydel/lyedit drive layer state through the compositor;
// lytween/tweenset/anime animate it; trans/uitrans crossfade between
// retained frames; lyevent registers click/rollover handlers; lyshader
// loads the per-layer GLSL. lydel additionally drops the dependent state
// (videos / E-mote players / message pages / event registrations of the
// deleted subtree).
#include "script/lua_engine.h"
#include "render/compositor.h"
#include "render/emote_player.h"
#include "render/video_player.h"
#include "render/stb_image.h"
#include "log/logger.h"
#include <cstdlib>
#include <string>
#include <vector>

namespace artc {

bool LuaEngine::TagLyrenameRaw(lua_State *L) {
    lua_getfield(L, 2, "id");
    const char *id = lua_tostring(L, -1);
    lua_getfield(L, 2, "to");
    const char *to = lua_tostring(L, -1);
    if (id && to && compositor_) compositor_->RenameLayer(id, to);
    lua_pop(L, 2);
    return true;
}

bool LuaEngine::TagLyShader(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    if(m.count("id") && m.count("file"))
        compositor_->LoadShader(m.at("id"),ResolvePackPath(m.at("file")));
    return true;
}

bool LuaEngine::TagLyEvent(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    // The framework emits click/rollover/rollout lyevent tags for the
    // SAME layer id; only the click registration carries the action
    // handler (function=btn_clickex) plus over/click attrs. Keep it
    // and ignore the siblings so `function` isn't overwritten.
    if (m["type"] != "click" && lyevents_.count(m["id"]))
        return true;
    StoreLyevent(m["id"], m);
    return true;
}

bool LuaEngine::TagLyCreate(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id") || !m.count("file")) return false;
    if (packs_) {
        compositor_->SetPackManager(packs_);
        compositor_->LoadImage(m["id"], ResolvePackPath(m["file"]));
    }
    return true;
}

bool LuaEngine::TagLyProp(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    if(m.count("intermediate_render_mask"))
        m["intermediate_render_mask"]=ResolvePackPath(m["intermediate_render_mask"]);
    compositor_->SetProps(m["id"], m);
    return true;
}

bool LuaEngine::TagTweenSet(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    compositor_->BeginTweenSet();
    return true;
}

bool LuaEngine::TagTweenSetEnd(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    compositor_->EndTweenSet(NowMs());
    return true;
}

// lytween — timed layer-property animation.
bool LuaEngine::TagLyTween(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    // KrKr2-Next: real tween (see Compositor::AddTween).
    compositor_->AddTween(m["id"], m, NowMs());
    return true;
}

bool LuaEngine::TagLyTweenDel(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    compositor_->DeleteTweens(m["id"]);
    return true;
}

bool LuaEngine::TagLyDel(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    compositor_->DeleteLayer(m["id"]);
    // Drop click handlers of the deleted subtree (like the title
    // button group "500"): otherwise stale buttons registered via
    // lyevent keep receiving hit-test hits inside the story.
    const std::string lid = m["id"];
    const std::string pre = lid + ".";
    for(auto it=videos_.begin();it!=videos_.end();) {
        if(it->first==lid || it->first.compare(0,pre.size(),pre)==0) it=videos_.erase(it);
        else ++it;
    }
    // E-mote players own their scene subtree; dropping the registry
    // entry alone would let the next frame re-create its layers under
    // a deleted parent.
    for(auto it=emotes_.begin();it!=emotes_.end();) {
        if(it->first==lid || it->first.compare(0,pre.size(),pre)==0) {
            it->second->RemoveLayers(*compositor_,it->first);
            it=emotes_.erase(it);
        } else ++it;
    }
    for (auto it = msg_text_.begin(); it != msg_text_.end();) {
        if (it->first == lid || it->first.compare(0, pre.size(), pre) == 0)
            it = msg_text_.erase(it);
        else ++it;
    }
    for (auto it = lyevents_.begin(); it != lyevents_.end();) {
        if (it->first == lid ||
            it->first.compare(0, pre.size(), pre) == 0)
            it = lyevents_.erase(it);
        else
            ++it;
    }
    return true;
}

bool LuaEngine::TagFlip(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    compositor_->Draw();
    return true;
}

// [trans] — retain the previous scene and start its transition.
bool LuaEngine::TagTrans(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    transition_wait_ = true;
    // KrKr2-Next: crossfade from the last presented frame. type=1 is
    // a plain fade; rule= names an 8-bit rule image in the pack.
    int time = 0;
    if (m.count("time")) time = std::atoi(m["time"].c_str());
    if (time > 0) {
        std::vector<uint8_t> rule;
        int rw = 0, rh = 0;
        const auto rit = m.find("rule");
        if (rit != m.end() && !rit->second.empty() && packs_) {
            std::vector<uint8_t> png;
            std::string path = ResolvePackPath(rit->second);
            if (!packs_->Read(path, png)) packs_->Read(path + ".png", png);
            if (!png.empty()) {
                int ch = 0;
                uint8_t *px = stbi_load_from_memory(png.data(), (int)png.size(), &rw, &rh, &ch, 1);
                if (px) {
                    rule.assign(px, px + static_cast<size_t>(rw) * rh);
                    stbi_image_free(px);
                } else {
                    rw = rh = 0;
                }
            }
        }
        int vague = 0;
        if (m.count("vague")) vague = std::atoi(m["vague"].c_str());
        compositor_->BeginTransition(NowMs(), time, rule, rw, rh, vague);
    }
    return true;
}

bool LuaEngine::TagLyEdit(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    // Replace a layer's image and/or restate its transform/alpha.
    if (m.count("file")) compositor_->LoadImage(m["id"], ResolvePackPath(m["file"]));
    std::map<std::string, std::string> props;
    for (const char *k : {"left", "top", "alpha", "clip", "xscale", "yscale"})
        if (m.count(k)) props[k] = m[k];
    if (!props.empty()) compositor_->SetProps(m["id"], props);
    return true;
}

bool LuaEngine::TagAnime(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const std::string id = m.count("id") ? m["id"] : "";
    const std::string mode = m.count("mode") ? m["mode"] : "init";
    const std::string file = m.count("file") ? ResolvePackPath(m["file"]) : "";
    const int time = std::atoi((m.count("time") ? m["time"]
                               : (m.count("0") ? m["0"] : "0")).c_str());
    const int loop = m.count("loop") ? std::atoi(m["loop"].c_str()) : -1;
    std::map<std::string, std::string> props;
    for (const char *k : {"left", "top", "alpha", "clip", "anchorx", "anchory",
                          "xscale", "yscale"})
        if (m.count(k)) props[k] = m[k];
    compositor_->SetAnimeFrame(id, mode, file, time, loop, props, NowMs());
    return true;
}

bool LuaEngine::TagUiTrans(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const std::string t = m.count("time") ? m["time"] : (m.count("0") ? m["0"] : "500");
    const int time = std::atoi(t.c_str());
    transition_wait_ = true;
    if (time > 0) compositor_->BeginTransition(NowMs(), time, {}, 0, 0, 0);
    return true;
}

bool LuaEngine::TagLyDrag(const std::string &tag, TagAttrs &m) {
    if (!compositor_ || !m.count("id")) return false;
    compositor_->SetProps(m["id"], {{"draggable", "1"}});
    return true;
}

bool LuaEngine::TagHide(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const bool allow = !m.count("allow") || m["allow"] != "0";
    // window= is a comma-separated layer list hidden while allow=1.
    if (m.count("window")) {
        const std::string &list = m.at("window");
        auto trim = [](std::string v) {
            size_t a = 0, b = v.size();
            while (a < b && std::isspace(static_cast<unsigned char>(v[a]))) ++a;
            while (b > a && std::isspace(static_cast<unsigned char>(v[b - 1]))) --b;
            return v.substr(a, b - a);
        };
        size_t s = 0;
        while (s <= list.size()) {
            size_t e = list.find(',', s);
            std::string id = trim(list.substr(s, e == std::string::npos ? std::string::npos : e - s));
            if (!id.empty())
                compositor_->SetProps(id, {{"visible", allow ? "0" : "1"}});
            if (e == std::string::npos) break;
            s = e + 1;
        }
    }
    const bool want_hidden = !allow;
    if (hidden_ != want_hidden) {
        hidden_ = want_hidden;
        FireNamedEvent(want_hidden ? "onhidein" : "onhideout");
    }
    return true;
}

} // namespace artc
