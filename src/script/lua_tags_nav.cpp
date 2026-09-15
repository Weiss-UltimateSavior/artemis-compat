// lua_tags_nav.cpp — script-flow control and system save.
//
// jump/call/stop/return drive the native runner through the host hooks;
// reset/exit terminate the flow; wait/wt gate the runner on time, input,
// animation or a specific voice; save/load/takess/savess persist and
// restore the engine state.
#include "script/lua_engine.h"
#include "render/compositor.h"
#include "render/video_player.h"
#include "audio/audio_channels.h"
#include "util/save_storage.h"
#include "log/logger.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

namespace artc {

bool LuaEngine::TagVideo(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const bool fullscreen=m["id"].empty();
    const std::string id=fullscreen ? "2147483647.artc_movie" : m["id"];
    if(m["file"].empty()) { videos_.erase(id); return true; }
    auto bytes=std::make_shared<std::vector<uint8_t>>();
    const std::string file=ResolvePackPath(m["file"]);
    if(!packs_ || !packs_->Read(file,*bytes)) {
        Log(kLogError,"video: file not found: "+file); return true;
    }
    videos_.erase(id);
    auto player=std::make_unique<VideoPlayer>(*compositor_,*audio_);
    const auto volume=vars_.find("s.videovol");
    const int gain=volume==vars_.end() ? 1000 : std::atoi(volume->second.c_str());
    if(!player->Start(bytes,id,m["loop"]=="1",fullscreen,gain,NowMs())) {
        Log(kLogError,"video: decoder failed: "+file); return true;
    }
    videos_[id]=std::move(player);
    if(fullscreen) {
        video_wait_=id; video_skip_=std::atoi(m["skip"].c_str());
        SetWaiting(true);
    }
    Log(kLogInfo,"video: playing "+file+(fullscreen ? " fullscreen" : " layer="+id));
    return true;
}

// Official wait semantics (spec/tag/script/wait.md): suspend the script
// until a user input (input=1/2) and/or the time (ms) elapses. `scenario`
// and trans_flag wait for active animation. The story page's separate click
// barrier is established by the `@` tag.
bool LuaEngine::TagWait(const std::string &tag, TagAttrs &m) {
    const std::string sc = m["scenario"];
    const bool after_trans = transition_wait_;
    if (!sc.empty() || after_trans) {
        // KrKr2-Next: tweens/transitions now run for real — hold the
        // runner for their remaining time (a tap still skips it).
        transition_wait_ = false;
        // A wait that follows [trans] is the kernel's wt: it holds the
        // script for the TRANSITION's remaining time, not for every
        // pending tween. Slow decorative tweens started under the trans
        // (e.g. a 150s background pan at a route intro) are meant to keep
        // running beneath the dialogue — waiting on max pending animation
        // serialized the whole intro behind the pan and locked input.
        double pending = 0;
        if (compositor_) {
            if (after_trans)
                pending = compositor_->TransitionRemainingMs(NowMs());
            else
                pending = std::max(
                    compositor_->PendingAnimationMs(NowMs()),
                    compositor_->PendingTextMs(NowMs()));
        }
        if (pending > 1) SetTimedWait(static_cast<int>(std::min(pending, 2147483647.0)), m["input"] != "0");
        return true;
    }
    int time = 0;
    const auto it = m.find("time");
    if (it != m.end()) time = std::atoi(it->second.c_str());
    const auto i0 = m.find("0");
    if (time == 0 && i0 != m.end()) time = std::atoi(i0->second.c_str());
    const std::string in = m["input"];
    // KrKr2-Next: se=N waits for that voice to end (input may still skip).
    const std::string se = m["se"];
    if (!se.empty()) {
        if (sounds_ && sounds_->IsPlaying(se)) {
            wait_se_key_ = se;
            se_wait_ = true;
            SetWaiting(true);
            wait_accept_input_ = in == "1" || in == "2";
            if (time > 0) SetTimedWait(time, wait_accept_input_);
        } else if (time > 0) {
            SetTimedWait(time);
        }
        return true;
    }
    if (time > 0) {
        SetTimedWait(time, in == "1" || in == "2");
    } else {
        // `input` permits skipping an existing wait; it does not create
        // a click barrier. Framework wt() emits {wait,input=1}, sometimes
        // twice after a transition. The second must complete immediately
        // when there is no animation left. `@` establishes a click wait.
        const double pending = compositor_
            ? compositor_->PendingAnimationMs(NowMs()) : 0;
        if (pending > 1) SetTimedWait(static_cast<int>(std::min(pending, 2147483647.0)), in == "1" || in == "2");
    }
    return true;
}

// A [stop]/[return] arriving as a tag (e.g. estag_call's final stop) halts
// the native runner through the host hook. The tag name ("stop" or
// "return") is passed so the host can distinguish the real engine's
// "stop → 停机等待 eqtag 排水" (pause, don't pop while a wait is queued)
// from a [return] (always pops the frame).
bool LuaEngine::TagStopReturn(const std::string &tag, TagAttrs &m) {
    if (!stop_handler_) return false;
    if (tag == "stop" && auto_stop_stop_) SetAutoMode(false);
    stop_handler_(tag);
    return true;
}

// estag chains are driven by `call system/script.asb label=estagNN` tags
// (estag("...") → e:tag{"call", ...}); nesting chains use the runner
// call-stack (estag_call chains start inside each other — e.g. uitrans
// starts its own mid-way).
bool LuaEngine::TagJump(const std::string &tag, TagAttrs &m) {
    if (!jump_handler_ || m["file"].empty()) return false;
    jump_handler_(ResolvePackPath(m["file"]), m["label"]);
    return true;
}

bool LuaEngine::TagCall(const std::string &tag, TagAttrs &m) {
    if (!call_handler_ || m["file"].empty()) return false;
    call_handler_(ResolvePackPath(m["file"]), m["label"]);
    return true;
}

// reset = engine reboot (official spec: 重启引擎). The language-selection
// flow ends with estag{uitrans, syssave, reset}: after the player picks a
// language the engine restarts so the boot chain re-runs with the new
// config. syssave() only *queues* the [save] tag (eqtag) — a reboot would
// drop that queue, so flush the variable bank to disk first.
bool LuaEngine::TagReset(const std::string &tag, TagAttrs &m) {
    SaveSystemData();
    RequestReset();
    return true;
}

// [exit] — title exit dialog YES → sv.go_exit → ui.asb *go_exit → engine
// terminates the process (host loop + Android activity finish).
bool LuaEngine::TagExit(const std::string &tag, TagAttrs &m) {
    RequestExit();
    return true;
}

// [save] — the framework's syssave() ends with eqtag{"save"}: persist the
// script variable bank (fsave_pluto values) to disk so a [reset] reboot
// can restore them (language config, system data, …).
bool LuaEngine::TagSave(const std::string &tag, TagAttrs &m) {
    if(m.count("file") && !m.at("file").empty())SaveSnapshot(m.at("file"));
    else SaveSystemData();
    return true;
}

bool LuaEngine::TagLoad(const std::string &tag, TagAttrs &m) {
    if (m.count("file")) LoadSnapshot(m.at("file"));
    return true;
}

bool LuaEngine::TagTakeSs(const std::string &tag, TagAttrs &m) {
    save_image_={};
    if(!compositor_ || !compositor_->Snapshot(save_image_))
        Log(kLogWarn,"takess: no retained scene to capture");
    return true;
}

bool LuaEngine::TagSaveSs(const std::string &tag, TagAttrs &m) {
    auto file=m["file"];
    if(file.empty()) {Log(kLogError,"savess: missing file");return true;}
    if(file.size()<4 || file.substr(file.size()-4)!=".png")file+=".png";
    auto dimension=[&](const char* name,int fallback) {
        auto p=m.find(name);if(p==m.end())return fallback;
        char* end=nullptr;const double n=std::strtod(p->second.c_str(),&end);
        return end==p->second.c_str()+p->second.size() && std::isfinite(n) && n>=1 && n<=8192 && n==std::floor(n)?int(n):0;
    };
    const int width=dimension("width",save_image_.width),height=dimension("height",save_image_.height);
    const auto path=SavePath(save_dir_,file);std::vector<uint8_t> png;
    if(path.empty() || !save_image_.EncodePng(width,height,png) || !WriteSaveFile(path,png))
        Log(kLogError,"savess: cannot save scene thumbnail "+file);
    else Log(kLogInfo,"savess: wrote "+file+" "+std::to_string(width)+"x"+std::to_string(height));
    return true;
}

// [wt] waits for running tweens/transitions.
bool LuaEngine::TagWt(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    const double pending = compositor_->PendingAnimationMs(NowMs());
    if (pending > 1) SetTimedWait(static_cast<int>(std::min(pending, 2147483647.0)));
    return true;
}

// Recognized engine-informational / config tags: the framework owns their
// UI, so store nothing and let the script continue instead of dispatching
// them as unknown.
bool LuaEngine::TagNoOp(const std::string &tag, TagAttrs &m) {
    if (!compositor_) return false;
    return true;
}

} // namespace artc
