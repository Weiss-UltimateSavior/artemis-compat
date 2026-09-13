#include "render/compositor.h"
#include "render/line_break.h"
#include "pack/pack_manager.h"
#include "pack/pf8_reader.h"
#include "log/logger.h"

// KrKr2-Next: the GLES2 compositor is also used on OpenHarmony (the host
// bridge makes an EGL context current before driving it). ARTC_HAS_GLES is
// set by cpp/artemis/CMakeLists.txt for every platform with a GLES2 sysroot.
#if defined(__ANDROID__) && !defined(ARTC_HAS_GLES)
#define ARTC_HAS_GLES 1
#endif

#if defined(ARTC_HAS_GLES)
#include <GLES2/gl2.h>
#define STB_IMAGE_IMPLEMENTATION
#include "render/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "render/stb_truetype.h"
#endif

#include <algorithm>
#include <cmath>
#include <set>
#include <cstdio>
#include <limits>
#include "script/save_storage.h"

namespace artc {

bool Compositor::LoadShader(const std::string& id,const std::string& file) {
    std::vector<uint8_t> bytes;
    if(!packs_ || !packs_->Read(file,bytes)) {
        Log(kLogError,"lyshader: cannot read "+file);return false;
    }
    const bool ok=shaders_.Load(id,std::string(bytes.begin(),bytes.end()));
    Log(ok?kLogInfo:kLogError,"lyshader: "+id+(ok?" loaded ":" failed ")+file);
    if(ok)++revision_;
    return ok;
}


// Artemis layer z is a two-level sort. The leading integer is the primary
// layer ("600", "1", "-273"); within the same primary layer the SECOND id
// segment breaks ties the way the engine draws it:
//   * numeric second segments sort by value — the message layer ("1.80…")
//     sits ABOVE the scene group ("1.0…") because 80 > 0;
//   * a letter second segment sorts ABOVE any number — the title button
//     ("500.d.…") sits ABOVE the title background ("500.0").
// Official z-order spec (spec/layer.md): stacking is decided by the LAYER ID.
//   * purely-numeric ids sort numerically (larger = drawn on top);
//   * an id with a leading number sorts by that number's VALUE first, then
//     by the string tail (so "500.d.1.0" > "500.0", and "1hoge" < "20hoge",
//     matching the documented example);
//   * an id with NO leading number ("bg", "froid"...) sorts to the bottom
//     (letter-only names are framework helper/background layers and must not
//     cover the numeric layer-set UI and scene layers).
int ZCmp(const std::string &a, const std::string &b) {
    auto lead = [](const std::string &s, int *start) -> long long {
        *start = 0;
        size_t i = 0;
        while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        if (i == 0) return -1;                 // no leading number
        long long v = 0;
        try { v = std::stoll(s.substr(0, i)); } catch (...) { v = 0; }
        *start = static_cast<int>(i);
        return v;
    };
    int sa = 0, sb = 0;
    const long long pa = lead(a, &sa);
    const long long pb = lead(b, &sb);
    if (pa == -1 && pb == -1) return a == b ? 0 : (a < b ? -1 : 1);
    if (pa != pb) return pa < pb ? -1 : 1;
    // same leading value: compare the whole strings lexicographically
    // (numeric tails compared on equal numbers fall back to the id text).
    if (a == b) return 0;
    return a < b ? -1 : 1;
}

// Layer hierarchy depth ("500.d.1.0" → 4 segments). Ties at the same z break
// toward deeper layers: the title background ("500.0") and its buttons
// ("500.d.*") share z=500, and the background must render *below* the buttons
// regardless of insertion order. Behavior observed from the real engine: the
// background is created after the buttons yet never covers them.
int SectionCount(const std::string &id) {
    int n = 1;
    for (const char c : id)
        if (c == '.') ++n;
    return n;
}

void Compositor::EffectiveRect(const Layer &l, float *ex, float *ey,
                               float *ea, bool *ev) const {
    float w, h;
    EffectiveRect(l, ex, ey, &w, &h, ea, ev);
}

// The reference CDisplayObject::ApplyPropertyToMatrix composes
// T(position) T(anchor) R(clockwise degrees) S(scale) S(reverse) T(-anchor).
// Compose the complete ancestor chain, so rotation and mirroring move face
// parts, glyphs and hit regions together, including nonuniform parent scales.
Compositor::Transform Compositor::EffectiveTransform(const Layer& l) const {
    std::vector<const Layer*> chain;
    std::string id=l.id;
    size_t dot;
    while ((dot=id.rfind('.'))!=std::string::npos) {
        id.resize(dot);
        for (const auto& parent:layers_)
            if (parent.id==id) { chain.push_back(&parent); break; }
    }
    Transform m;
    auto apply=[&](const Layer& n) {
        const float radians=std::remainder(n.rotate,360.f)*3.14159265358979323846f/180.f;
        float co=std::cos(radians), si=std::sin(radians);
        // Exact quadrants should not produce cracks or fail edge hit tests.
        if (std::abs(co)<1e-7f) co=0;
        if (std::abs(si)<1e-7f) si=0;
        const float sx=n.sx*(n.reverse_x ? -1.f : 1.f);
        const float sy=n.sy*(n.reverse_y ? -1.f : 1.f);
        const float a=co*sx, b=si*sx, c=-si*sy, d=co*sy;
        const float tx=n.x+n.ax-a*n.ax-c*n.ay;
        const float ty=n.y+n.ay-b*n.ax-d*n.ay;
        const auto origin=m.Point(tx,ty);
        const float ma=m.a*a+m.c*b, mb=m.b*a+m.d*b;
        const float mc=m.a*c+m.c*d, md=m.b*c+m.d*d;
        m.a=ma; m.b=mb; m.c=mc; m.d=md; m.tx=origin.first; m.ty=origin.second;
        m.alpha*=n.alpha; m.visible=m.visible && n.visible;
    };
    for (auto it=chain.rbegin();it!=chain.rend();++it) apply(**it);
    apply(l);
    const auto origin=m.Point(l.content_x,l.content_y);
    m.tx=origin.first; m.ty=origin.second;
    return m;
}

void Compositor::EffectiveRect(const Layer& l, float* ex, float* ey, float* ew,
                               float* eh, float* ea, bool* ev) const {
    const auto m=EffectiveTransform(l);
    *ea=m.alpha; *ev=m.visible;
    if (m.b==0 && m.c==0) {
        // Preserve the signed dimensions exposed by the scale-only API.
        *ex=m.tx; *ey=m.ty; *ew=m.a*l.w; *eh=m.d*l.h;
        return;
    }
    const auto p0=m.Point(0,0), p1=m.Point(l.w,0);
    const auto p2=m.Point(0,l.h), p3=m.Point(l.w,l.h);
    *ex=std::min({p0.first,p1.first,p2.first,p3.first});
    *ey=std::min({p0.second,p1.second,p2.second,p3.second});
    *ew=std::max({p0.first,p1.first,p2.first,p3.first})-*ex;
    *eh=std::max({p0.second,p1.second,p2.second,p3.second})-*ey;
}

bool Compositor::ContainsPoint(const Layer& l, float x, float y) const {
    const auto m=EffectiveTransform(l);
    const float det=m.a*m.d-m.b*m.c;
    if (!m.visible || !l.texture || !std::isfinite(det) || std::abs(det)<1e-8f) return false;
    x-=m.tx; y-=m.ty;
    const float local_x=(m.d*x-m.c*y)/det, local_y=(m.a*y-m.b*x)/det;
    return local_x>=0 && local_y>=0 && local_x<l.w && local_y<l.h;
}

bool Compositor::ParentDelta(const std::string& id, float dx, float dy, float* x, float* y) const {
    // An identity child with this id collects exactly the real layer's ancestors.
    Layer child; child.id=id;
    const auto m=EffectiveTransform(child);
    const float det=m.a*m.d-m.b*m.c;
    if (!std::isfinite(det) || std::abs(det)<1e-8f) return false;
    *x=(m.d*dx-m.c*dy)/det;
    *y=(m.a*dy-m.b*dx)/det;
    return true;
}

std::string Compositor::HitLayer(float x, float y) const {
    const Layer *best = nullptr;
    for (const auto &l : layers_) {
        if (!ContainsPoint(l,x,y)) continue;
        if (!best) best = &l;
        else {
            const int c = ZCmp(l.id, best->id);
            const bool lFront = c > 0 ||
                                (c == 0 &&
                                 SectionCount(l.id) > SectionCount(best->id));
            if (lFront) best = &l;
        }
    }
    return best ? best->id : std::string();
}

// [var system="get_layer_info"] — stored (relative) offsets and draggable
// bounds. slider_dragX reads left to derive the percentage, so we return the
// raw stored value, not the cascade-resolved absolute position.
// ---------------------------------------------------------------------------
// KrKr2-Next: tween engine (platform independent)
// ---------------------------------------------------------------------------
namespace {
enum Ease { kEaseLinear = 0, kEaseInQuad, kEaseOutQuad, kEaseInOutQuad,
            kEaseInCubic, kEaseOutCubic, kEaseInOutCubic, kEaseInSine,
            kEaseOutSine, kEaseInOutSine, kEaseOutBack, kEaseOutBounce };

int ParseEase(const std::string &name) {
    if (name.empty() || name == "none" || name == "linear") return kEaseLinear;
    if (name == "easein_quad") return kEaseInQuad;
    if (name == "easeout_quad") return kEaseOutQuad;
    if (name == "easeinout_quad") return kEaseInOutQuad;
    if (name == "easein_cubic") return kEaseInCubic;
    if (name == "easeout_cubic") return kEaseOutCubic;
    if (name == "easeinout_cubic") return kEaseInOutCubic;
    if (name == "easein_sine") return kEaseInSine;
    if (name == "easeout_sine") return kEaseOutSine;
    if (name == "easeinout_sine") return kEaseInOutSine;
    if (name == "easeout_back") return kEaseOutBack;
    if (name == "easeout_bounce") return kEaseOutBounce;
    // unknown curve families (elastic, quart, expo...) fall back by direction
    if (name.rfind("easein", 0) == 0 && name.find("out") == std::string::npos) return kEaseInQuad;
    if (name.rfind("easeout", 0) == 0) return kEaseOutQuad;
    return kEaseInOutQuad;
}

float ApplyEase(int ease, float t) {
    if (t <= 0) return 0;
    if (t >= 1) return 1;
    const float pi = 3.14159265f;
    switch (ease) {
    case kEaseInQuad: return t * t;
    case kEaseOutQuad: return 1 - (1 - t) * (1 - t);
    case kEaseInOutQuad: return t < 0.5f ? 2 * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) / 2;
    case kEaseInCubic: return t * t * t;
    case kEaseOutCubic: { const float u = 1 - t; return 1 - u * u * u; }
    case kEaseInOutCubic: return t < 0.5f ? 4 * t * t * t : 1 - (-2 * t + 2) * (-2 * t + 2) * (-2 * t + 2) / 2;
    case kEaseInSine: return 1 - std::cos(t * pi / 2);
    case kEaseOutSine: return std::sin(t * pi / 2);
    case kEaseInOutSine: return -(std::cos(pi * t) - 1) / 2;
    case kEaseOutBack: { const float c1 = 1.70158f, c3 = c1 + 1; const float u = t - 1;
                         return 1 + c3 * u * u * u + c1 * u * u; }
    case kEaseOutBounce: {
        const float n1 = 7.5625f, d1 = 2.75f;
        if (t < 1 / d1) return n1 * t * t;
        if (t < 2 / d1) { t -= 1.5f / d1; return n1 * t * t + 0.75f; }
        if (t < 2.5f / d1) { t -= 2.25f / d1; return n1 * t * t + 0.9375f; }
        t -= 2.625f / d1; return n1 * t * t + 0.984375f;
    }
    default: return t;
    }
}

float ToF(const std::string &s, float def = 0) {
    try { return s.empty() ? def : std::stof(s); } catch (...) { return def; }
}
} // namespace

void Compositor::SetTextTween(const std::string& id, const std::map<std::string, std::string>& attrs) {
    if (id.empty()) return;
    const auto get = [&](const char* key) { const auto it=attrs.find(key); return it==attrs.end() ? std::string() : it->second; };
    if (get("type") != "in") return;
    SetProps(id, {});
    for (auto& l : layers_) if (l.id == id) {
        if (get("mode") == "init") l.text_in.clear();
        else if (get("mode") == "add") {
            const auto param=get("param");
            if (param=="alpha" || param=="left" || param=="top")
                l.text_in.push_back({param, std::max(0.f,ToF(get("delay"))),
                    std::max(0.f,ToF(get("time"))), ToF(get("diff")), ParseEase(get("ease"))});
        }
        return;
    }
}

void Compositor::SetGlyphTimes(Layer& l, std::vector<TextGlyph>& glyphs, const std::string& text) {
    const bool append=text.compare(0,l.text.size(),l.text)==0;
    std::map<size_t,double> old_times;
    if(append) for(const auto& g:l.glyphs) old_times[g.order]=g.start_ms;
    double delay=0;
    for(const auto& t:l.text_in) delay=std::max(delay,t.delay_ms);
    const size_t first_new=old_times.empty() ? 0 : old_times.rbegin()->first+1;
    const double next=old_times.empty() ? now_ms_ : std::max(now_ms_,old_times.rbegin()->second+delay);
    for(auto& g:glyphs) {
        const auto old=old_times.find(g.order);
        g.start_ms=old!=old_times.end() ? old->second : next+(g.order-first_new)*delay;
    }
    l.glyphs=std::move(glyphs); l.text=text;
}

double Compositor::TextEnd(const Layer& l) {
    if (l.text_in.empty() || l.glyphs.empty()) return 0;
    double duration=0;
    for(const auto& tw:l.text_in) duration=std::max(duration,tw.time_ms);
    double end=0;
    for(const auto& g:l.glyphs) end=std::max(end,g.start_ms+duration);
    return end;
}

double Compositor::PendingTextMs(double now_ms) const {
    double end=now_ms;
    for(const auto& l:layers_) {
        float x,y,a; bool visible;
        EffectiveRect(l,&x,&y,&a,&visible);
        if(visible && a>0) end=std::max(end,TextEnd(l));
    }
    return end-now_ms;
}

bool Compositor::FinishText(double now_ms) {
    bool changed=false;
    for(auto& l:layers_) {
        float x,y,a; bool visible;
        EffectiveRect(l,&x,&y,&a,&visible);
        const double remaining=TextEnd(l)-now_ms;
        if (!visible || a<=0 || remaining<=0) continue;
        for(auto& g:l.glyphs) g.start_ms-=remaining;
        changed=true;
    }
    if(changed) ++revision_;
    return changed;
}

bool Compositor::ReadParam(const Layer &l, const std::string &param, float *v) {
    if (param == "alpha") { *v = l.alpha * 255.0f; return true; }
    if (param == "left" || param == "x") { *v = l.x; return true; }
    if (param == "top" || param == "y") { *v = l.y; return true; }
    if (param == "xscale") { *v = l.sx * 100.0f; return true; }
    if (param == "yscale") { *v = l.sy * 100.0f; return true; }
    if (param == "zoom") { *v = l.sx * 100.0f; return true; }
    if (param == "rotate") { *v = l.rotate; return true; }
    if (param == "w") { *v = l.w; return true; }
    if (param == "h") { *v = l.h; return true; }
    return false;
}

bool Compositor::ApplyParam(Layer &l, const std::string &param, float v) {
    if (param == "alpha") { l.alpha = std::clamp(v / 255.0f, 0.0f, 1.0f); return true; }
    if (param == "left" || param == "x") { l.x = v; l.own_pos = true; return true; }
    if (param == "top" || param == "y") { l.y = v; l.own_pos = true; return true; }
    if (param == "xscale") { l.sx = v / 100.0f; return true; }
    if (param == "yscale") { l.sy = v / 100.0f; return true; }
    if (param == "zoom") { l.sx = l.sy = v / 100.0f; return true; }
    if (param == "rotate") { l.rotate = v; return true; }
    if (param == "w") { l.w = v; return true; }
    if (param == "h") { l.h = v; return true; }
    return false;
}

void Compositor::AddTween(const std::string &id,
                          const std::map<std::string, std::string> &attrs, double now_ms) {
    auto get = [&](const char *k) -> std::string {
        const auto it = attrs.find(k);
        return it == attrs.end() ? std::string() : it->second;
    };
    std::string param = get("param");
    // `param` missing: the animated property is whichever known key carries
    // a "from,to" pair (tween{ id, alpha="255,0" } style).
    if (param.empty()) {
        for (const char *k : {"alpha", "left", "top", "x", "y", "xscale", "yscale", "zoom", "rotate", "w", "h"}) {
            if (attrs.count(k)) { param = k; break; }
        }
    }
    if (param.empty()) return;
    Tween tw;
    tw.id = id;
    tw.param = param;
    tw.time_ms = std::max(0.0f, ToF(get("time"), 0));
    tw.delay_ms = std::max(0.0f, ToF(get("delay"), 0));
    const int loop = static_cast<int>(ToF(get("loop"), 0));
    const int yoyo = static_cast<int>(ToF(get("yoyo"), 0));
    tw.repeat = std::max(-1, loop != 0 ? loop : yoyo);
    tw.yoyo = loop == 0 && yoyo != 0;
    tw.ease = ParseEase(get("ease"));
    tw.start_ms = now_ms;
    // from/to: explicit attrs win; else "<param>=a,b"; else current -> to.
    const std::string from = get("from"), to = get("to");
    const std::string pair = get(param.c_str());
    const size_t comma = pair.find(',');
    if (!to.empty()) tw.to = ToF(to);
    else if (comma != std::string::npos) tw.to = ToF(pair.substr(comma + 1));
    else tw.to = ToF(pair);
    if (!from.empty()) { tw.from = ToF(from); tw.from_current = false; }
    else if (comma != std::string::npos) { tw.from = ToF(pair.substr(0, comma)); tw.from_current = false; }
    if (collecting_tweens_) tween_set_.push_back(tw);
    else QueueTween(tw, true);
}

double Compositor::Tween::Duration() const {
    if (time_ms <= 0) return delay_ms;
    return repeat < 0 ? std::numeric_limits<double>::infinity() :
        delay_ms + time_ms * (1.0 + repeat);
}

float Compositor::Tween::FinalValue() const {
    return yoyo && repeat >= 0 && (repeat & 1) ? from : to;
}

void Compositor::QueueTween(Tween tw, bool replace) {
    if (replace) {
        tweens_.erase(std::remove_if(tweens_.begin(), tweens_.end(), [&](const Tween& old) {
            return old.id == tw.id && old.param == tw.param;
        }), tweens_.end());
    }
    bool found = false;
    for (auto &l : layers_) if (l.id == tw.id) { found = true; break; }
    if (!found) { Layer g; g.id = tw.id; layers_.push_back(g); }
    // Even zero-duration segments enter the queue, so a later segment can
    // resolve its implicit start from the preceding segment's final value.
    tweens_.push_back(tw);
    ++revision_;
}

void Compositor::BeginTweenSet() {
    if (!collecting_tweens_) { collecting_tweens_ = true; tween_set_.clear(); }
}

void Compositor::EndTweenSet(double now_ms) {
    if (!collecting_tweens_) return;
    collecting_tweens_ = false;
    std::map<std::pair<std::string,std::string>, double> ends;
    for (auto tw : tween_set_) {
        const auto key = std::make_pair(tw.id, tw.param);
        const auto previous = ends.find(key);
        tw.start_ms = previous == ends.end() ? now_ms : previous->second;
        ends[key] = tw.start_ms + tw.Duration();
        QueueTween(tw, previous == ends.end());
    }
    tween_set_.clear();
}

void Compositor::DeleteTweens(const std::string &id) {
    const std::string prefix = id + ".";
    for (auto it = tweens_.begin(); it != tweens_.end();) {
        if (it->id == id || it->id.compare(0, prefix.size(), prefix) == 0) {
            // cancel = jump to the end value (the framework deletes tweens
            // once their visual purpose is served, e.g. reveal done)
            for (auto &l : layers_) if (l.id == it->id) ApplyParam(l, it->param, it->FinalValue());
            it = tweens_.erase(it);
            ++revision_;
        } else {
            ++it;
        }
    }
}

bool Compositor::Update(double now_ms) {
    bool changed = PendingTextMs(now_ms_) > 0;
    now_ms_ = now_ms;
    for (auto it = tweens_.begin(); it != tweens_.end();) {
        Tween &tw = *it;
        const double since = now_ms - tw.start_ms;
        if (since < tw.delay_ms) { ++it; continue; }
        Layer *target = nullptr;
        for (auto &l : layers_) if (l.id == tw.id) { target = &l; break; }
        if (!target) { it = tweens_.erase(it); continue; }
        if (!tw.started) {
            tw.started = true;
            if (tw.from_current) {
                float cur = tw.to;
                if (ReadParam(*target, tw.param, &cur)) tw.from = cur;
            }
        }
        const double run = since - tw.delay_ms;
        const bool done = tw.time_ms <= 0 ||
            (tw.repeat >= 0 && run >= tw.time_ms * (1.0 + tw.repeat));
        float v = tw.FinalValue();
        if (!done) {
            const double cycle = std::floor(run / tw.time_ms);
            const float t = static_cast<float>(std::fmod(run, tw.time_ms) / tw.time_ms);
            const bool reverse = tw.yoyo && std::fmod(cycle, 2.0) >= 1.0;
            const float from = reverse ? tw.to : tw.from;
            const float to = reverse ? tw.from : tw.to;
            v = from + (to - from) * ApplyEase(tw.ease, t);
        }
        ApplyParam(*target, tw.param, v);
        changed = true;
        if (done) it = tweens_.erase(it); else ++it;
    }
    if (trans_active_) {
        if (now_ms - trans_start_ms_ >= trans_time_ms_) trans_active_ = false;
        changed = true;   // the overlay fades every frame (or just went away)
    }
    if (changed) ++revision_;
    return changed;
}

double Compositor::PendingAnimationMs(double now_ms) const {
    double remain = 0;
    for (const Tween &tw : tweens_) {
        const double end = tw.start_ms + tw.Duration();
        if (end - now_ms > remain) remain = end - now_ms;
    }
    if (trans_active_) {
        const double end = trans_start_ms_ + trans_time_ms_;
        if (end - now_ms > remain) remain = end - now_ms;
    }
    return remain < 0 ? 0 : remain;
}

std::string Compositor::DescribeDrawList(size_t max_layers) const {
    std::vector<const Layer *> sorted;
    for (const auto &l : layers_) sorted.push_back(&l);
    std::stable_sort(sorted.begin(), sorted.end(), [](const Layer *a, const Layer *b) {
        const int c = ZCmp(a->id, b->id);
        if (c != 0) return c < 0;
        return SectionCount(a->id) < SectionCount(b->id);
    });
    std::string out = "layers=" + std::to_string(layers_.size()) +
                      " tweens=" + std::to_string(tweens_.size()) +
                      " trans=" + std::to_string(trans_active_ ? 1 : 0) + " |";
    size_t n = 0;
    for (const Layer *l : sorted) {
        float ex, ey, ew, eh, ea; bool ev;
        EffectiveRect(*l, &ex, &ey, &ew, &eh, &ea, &ev);
        if (!ev || !l->texture) continue;
        if (n++ >= max_layers) { out += " ..."; break; }
        char buf[160];
        std::snprintf(buf, sizeof(buf), " %s(%d,%d %dx%d a=%.2f)", l->id.c_str(), (int)ex, (int)ey,
                      (int)ew, (int)eh, ea);
        out += buf;
    }
    return out;
}

std::vector<std::string> Compositor::HitLayers(float x, float y) const {
    std::vector<const Layer *> hits;
    for (const auto &l : layers_) {
        if (!ContainsPoint(l,x,y)) continue;
        hits.push_back(&l);
    }
    // topmost first: higher z (ZCmp) then deeper id wins
    std::stable_sort(hits.begin(), hits.end(), [](const Layer *a, const Layer *b) {
        const int c = ZCmp(a->id, b->id);
        if (c != 0) return c > 0;
        return SectionCount(a->id) > SectionCount(b->id);
    });
    std::vector<std::string> ids;
    ids.reserve(hits.size());
    for (const Layer *l : hits) ids.push_back(l->id);
    return ids;
}

Compositor::LayerInfo Compositor::GetLayerInfo(const std::string &id) const {
    LayerInfo info;
    for (const auto &l : layers_) {
        if (l.id != id) continue;
        info.found = true;
        info.left = l.x;
        info.top = l.y;
        info.width = l.w;
        info.height = l.h;
        info.draggable = l.draggable;
        info.has_dragarea = l.has_dragarea;
        info.drag_l = l.drag_l;
        info.drag_t = l.drag_t;
        info.drag_r = l.drag_r;
        info.drag_b = l.drag_b;
        break;
    }
    return info;
}

void Compositor::DumpRects() {
    for (const Layer &l : layers_) {
        if (!l.visible || !l.texture) continue;
        if (!dumped_.insert(l.id).second) continue;
        float ex, ey, ea; bool ev;
        EffectiveRect(l, &ex, &ey, &ea, &ev);
        if (!ev) continue;
        Log(kLogInfo, "rect: " + l.id + " x=" + std::to_string(ex) +
                          " y=" + std::to_string(ey) + " w=" + std::to_string(l.w) +
                          " h=" + std::to_string(l.h) + " z=" + std::to_string(l.z) +
                          " a=" + std::to_string(ea));
    }
}

#if defined(ARTC_HAS_GLES)

namespace {
const char *kVs = R"(attribute vec2 a_pos;
attribute vec2 a_uv;
attribute float a_opacity;
uniform vec2 u_screen;
uniform float u_top_down;
varying vec2 v_uv;
varying float v_opacity;
void main() {
    vec2 clip = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,
                     1.0 - a_pos.y / u_screen.y * 2.0);
    clip.y=mix(clip.y,-clip.y,u_top_down);
    gl_Position = vec4(clip, 0.0, 1.0);
    v_uv = a_uv;
    v_opacity = a_opacity;
})";

const char *kFs = R"(precision mediump float;
varying vec2 v_uv;
varying float v_opacity;
uniform sampler2D u_tex;
uniform float u_alpha;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    gl_FragColor = vec4(c.rgb, c.a * u_alpha * v_opacity);
})";

// KrKr2-Next: [trans] overlay — the previous frame fades out over the new
// layer state. Plain fade uses u_t; rule transitions threshold the rule
// image (dark pixels change first) with a `vague` soft edge.
const char *kTransFs = R"(precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform sampler2D u_rule;
uniform float u_t;
uniform float u_vague;
uniform int u_use_rule;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    float a;
    if (u_use_rule == 1) {
        float r = texture2D(u_rule, vec2(v_uv.x, 1.0 - v_uv.y)).r;
        float edge = mix(-u_vague, 1.0 + u_vague, u_t);
        a = smoothstep(edge - u_vague, edge + u_vague, r);
    } else {
        a = 1.0 - u_t;
    }
    gl_FragColor = vec4(c.rgb, c.a * a);
})";

// Artemis layer ids sort by their leading integer ("600.4.0" → 600,
// "1.80.mw" → 1, "-273" → -273); non-numeric ids keep insertion order.
uint32_t CompileShader(uint32_t type, const char *src) {
    uint32_t s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    return s;
}
} // namespace

bool Compositor::InitGl() {
    if (gl_ready_) return true;
    prog_.program = glCreateProgram();
    uint32_t vs = CompileShader(GL_VERTEX_SHADER, kVs);
    uint32_t fs = CompileShader(GL_FRAGMENT_SHADER, kFs);
    glAttachShader(prog_.program, vs);
    glAttachShader(prog_.program, fs);
    glBindAttribLocation(prog_.program,0,"a_pos");
    glBindAttribLocation(prog_.program,1,"a_uv");
    glBindAttribLocation(prog_.program,2,"a_opacity");
    glLinkProgram(prog_.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    prog_.a_pos = glGetAttribLocation(prog_.program, "a_pos");
    prog_.a_uv = glGetAttribLocation(prog_.program, "a_uv");
    prog_.a_opacity = glGetAttribLocation(prog_.program, "a_opacity");
    prog_.u_screen = glGetUniformLocation(prog_.program, "u_screen");
    prog_.u_top_down = glGetUniformLocation(prog_.program,"u_top_down");
    prog_.u_tex = glGetUniformLocation(prog_.program, "u_tex");
    prog_.u_alpha = glGetUniformLocation(prog_.program, "u_alpha");

    tprog_.program = glCreateProgram();
    uint32_t tvs = CompileShader(GL_VERTEX_SHADER, kVs);
    uint32_t tfs = CompileShader(GL_FRAGMENT_SHADER, kTransFs);
    glAttachShader(tprog_.program, tvs);
    glAttachShader(tprog_.program, tfs);
    glLinkProgram(tprog_.program);
    glDeleteShader(tvs);
    glDeleteShader(tfs);
    tprog_.a_pos = glGetAttribLocation(tprog_.program, "a_pos");
    tprog_.a_uv = glGetAttribLocation(tprog_.program, "a_uv");
    tprog_.u_screen = glGetUniformLocation(tprog_.program, "u_screen");
    tprog_.u_tex = glGetUniformLocation(tprog_.program, "u_tex");
    tprog_.u_rule = glGetUniformLocation(tprog_.program, "u_rule");
    tprog_.u_t = glGetUniformLocation(tprog_.program, "u_t");
    tprog_.u_vague = glGetUniformLocation(tprog_.program, "u_vague");
    tprog_.u_use_rule = glGetUniformLocation(tprog_.program, "u_use_rule");
    gl_ready_ = true;
    return true;
}

// Copy the composited stage (the current viewport) into last_frame_tex_ so
// the next [trans] can fade it out. GPU-side copy; nothing is read back.
void Compositor::CaptureFrame() {
    if (!gl_ready_ || !scene_fbo_) return;
    GLint previous_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glActiveTexture(GL_TEXTURE0);
    if (!last_frame_tex_) {
        glGenTextures(1, &last_frame_tex_);
        glBindTexture(GL_TEXTURE_2D, last_frame_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    } else {
        glBindTexture(GL_TEXTURE_2D, last_frame_tex_);
    }
    // GLES2 rejects copying more components than the framebuffer stores:
    // pick RGB when the surface has no alpha plane.
    glGetError();   // clear any stale error before the copy
    glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 0, 0, stage_w_, stage_h_, 0);
    trans_have_frame_ = glGetError() == GL_NO_ERROR;
    glBindFramebuffer(GL_FRAMEBUFFER, previous_fbo);
}

bool Compositor::BeginTransition(double now_ms, int time_ms, const std::vector<uint8_t> &rule,
                                 int rule_w, int rule_h, int vague) {
    // Window back buffers are undefined after eglSwapBuffers. The retained
    // scene remains valid on Android, OHOS and pbuffer hosts alike.
    CaptureFrame();
    if (!gl_ready_ || time_ms <= 0 || !trans_have_frame_) return false;
    if (trans_rule_tex_) { glDeleteTextures(1, &trans_rule_tex_); trans_rule_tex_ = 0; }
    if (!rule.empty() && rule_w > 0 && rule_h > 0 &&
        rule.size() >= static_cast<size_t>(rule_w) * rule_h) {
        // expand 8-bit rule to RGBA (GLES2 has no single-channel float path
        // worth the trouble; LUMINANCE would also work)
        std::vector<uint8_t> rgba(static_cast<size_t>(rule_w) * rule_h * 4);
        for (size_t i = 0; i < static_cast<size_t>(rule_w) * rule_h; ++i) {
            rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = rule[i];
            rgba[i * 4 + 3] = 255;
        }
        trans_rule_tex_ = CreateTexture(rgba.data(), rule_w, rule_h);
    }
    trans_vague_ = vague < 0 ? 0 : (vague > 255 ? 1.0f : vague / 255.0f);
    trans_start_ms_ = now_ms;
    trans_time_ms_ = time_ms;
    trans_active_ = true;
    ++revision_;
    return true;
}

void Compositor::DrawTransitionOverlay() {
    if (!trans_active_ || !last_frame_tex_) return;
    float t = trans_time_ms_ > 0 ? static_cast<float>((now_ms_ - trans_start_ms_) / trans_time_ms_) : 1.0f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    glUseProgram(tprog_.program);
    glUniform2f(tprog_.u_screen, (float)stage_w_, (float)stage_h_);
    glUniform1i(tprog_.u_tex, 0);
    glUniform1i(tprog_.u_rule, 1);
    glUniform1f(tprog_.u_t, t);
    glUniform1f(tprog_.u_vague, trans_vague_ > 0.002f ? trans_vague_ : 0.002f);
    glUniform1i(tprog_.u_use_rule, trans_rule_tex_ ? 1 : 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, trans_rule_tex_ ? trans_rule_tex_ : last_frame_tex_);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, last_frame_tex_);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // Captured frame rows are bottom-up: sample v=1 at the stage top.
    const float W = (float)stage_w_, H = (float)stage_h_;
    float verts[16] = {
        0, 0, 0, 1,   W, 0, 1, 1,
        0, H, 0, 0,   W, H, 1, 0,
    };
    glVertexAttribPointer(tprog_.a_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
    glEnableVertexAttribArray(tprog_.a_pos);
    glVertexAttribPointer(tprog_.a_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
    glEnableVertexAttribArray(tprog_.a_uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisable(GL_BLEND);
    glUseProgram(prog_.program);
}

uint32_t Compositor::CreateTexture(const uint8_t *pixels, int w, int h) {
    uint32_t tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return tex;
}

bool Compositor::LoadImage(const std::string &id, const std::string &file) {
    ++revision_;
    if (!gl_ready_ || !packs_) return false;
    // Scripts usually omit extensions (:bg/black); the real engine matches
    // packed assets by trying the common image extensions.
    static const char *kExtensions[] = {"", ".png", ".jpg", ".jpeg"};
    std::vector<uint8_t> bytes;
    std::string resolved;
    for (const char *ext : kExtensions) {
        resolved = file + ext;
        if (packs_->Read(resolved, bytes) || ReadSaveFile(SavePath(save_directory_,resolved),bytes)) break;
    }
    if (bytes.empty()) {
        Log(kLogWarn, "lyc: image not found: " + file);
        return false;
    }
    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &channels, 4);
    if (!pixels) {
        Log(kLogWarn, "lyc: decode failed: " + file);
        return false;
    }
    uint32_t tex = CreateTexture(pixels, w, h);
    stbi_image_free(pixels);

    // replace or create layer
    for (auto &l : layers_) {
        if (l.id == id) {
            if (l.texture) glDeleteTextures(1, &l.texture);
            l.texture = tex;
            l.tex_w = w; l.tex_h = h;
            l.content_x = l.content_y = 0;
            l.glyphs.clear(); l.text.clear();
            // A replacement is a new image surface. Face differences often
            // have different bounding boxes; retaining the previous surface's
            // size stretches the new eyes/mouth away from their scripted origin.
            // Keep the layer transform, but reset image-local geometry. A lyprop
            // following lyc can apply a new sprite-sheet crop/display size.
            l.w = (float)w; l.h = (float)h;
            l.u0 = l.v0 = 0; l.u1 = l.v1 = 1;
            Log(kLogDebug, "lyc: replaced " + id);
            return true;
        }
    }
    Layer l;
    l.id = id;
    l.texture = tex;
    l.tex_w = w; l.tex_h = h;
    l.w = (float)w; l.h = (float)h;
    l.z = 0;
    layers_.push_back(l);
    Log(kLogDebug, "lyc: added " + id + " " + std::to_string(w) + "x" + std::to_string(h));
    return true;
}

void Compositor::LoadMask(const std::string& file) {
    if(file.empty() || masks_.count(file) || !gl_ready_ || !packs_)return;
    auto& mask=masks_[file];std::vector<uint8_t> bytes;
    for(const char* ext:{"",".png",".jpg",".jpeg"})if(packs_->Read(file+ext,bytes))break;
    int w=0,h=0,channels=0;
    if(bytes.empty() || bytes.size()>64*1024*1024 ||
       !stbi_info_from_memory(bytes.data(),int(bytes.size()),&w,&h,&channels) ||
       w<1 || h<1 || w>8192 || h>8192 || uint64_t(w)*h>16777216) {
        Log(kLogWarn,"intermediate mask unavailable: "+file);return;
    }
    auto* pixels=stbi_load_from_memory(bytes.data(),int(bytes.size()),&w,&h,&channels,4);
    if(!pixels){Log(kLogWarn,"intermediate mask decode failed: "+file);return;}
    mask={CreateTexture(pixels,w,h),w,h};stbi_image_free(pixels);
}

bool Compositor::LoadFont(const std::string &file) {
    if (!gl_ready_ || !packs_) return false;
    if (font_ready_ && file == font_path_) return true;
    std::vector<uint8_t> data;
    if (!packs_->Read(file, data)) {
        Log(kLogWarn, "font not found in packs: " + file);
        return false;
    }
    auto *info = new stbtt_fontinfo;
    const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info, data.data(), offset)) {
        Log(kLogError, "font init failed: " + file);
        delete info;
        return false;
    }
    delete static_cast<stbtt_fontinfo *>(font_info_);
    font_data_ = std::move(data);
    font_path_ = file;
    font_info_ = info;
    font_ready_ = true;
    Log(kLogInfo, "font loaded: " + file + " (" + std::to_string(font_data_.size()) + " B)");
    return true;
}

bool Compositor::SetPixels(const std::string& id, const uint8_t* rgba, int width, int height) {
    if(!rgba || width<=0 || height<=0 || width>8192 || height>8192) return false;
    SetProps(id,{});
    for(auto& l:layers_) if(l.id==id) {
        if(!l.texture || l.w==l.tex_w) l.w=width;
        if(!l.texture || l.h==l.tex_h) l.h=height;
        if(l.texture && l.tex_w==width && l.tex_h==height) {
            glBindTexture(GL_TEXTURE_2D,l.texture);
            glTexSubImage2D(GL_TEXTURE_2D,0,0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        } else {
            if(l.texture) glDeleteTextures(1,&l.texture);
            l.texture=CreateTexture(rgba,width,height);
        }
        l.tex_w=width; l.tex_h=height;
        l.glyphs.clear(); l.text.clear(); l.content_x=l.content_y=0;
        l.u0=l.v0=0; l.u1=l.v1=1;
        return l.texture!=0;
    }
    return false;
}

bool Compositor::SetText(const std::string &id, const std::string &text,
                         float size, uint32_t color, float wrapWidth,
                         const std::map<std::string, std::string>& style,
                         const std::vector<TextRuby>& ruby) {
    ++revision_;
    if (text.empty()) {
        for (auto& l : layers_) if (l.id == id) {
            l.glyphs.clear(); l.text.clear();
            if (l.texture) glDeleteTextures(1, &l.texture);
            l.texture = 0;
            l.tex_w = l.tex_h = 0;
            l.w = l.h = 0;
        }
        return true;
    }
    if (!font_ready_) return false;
    auto *info = static_cast<stbtt_fontinfo *>(font_info_);
    const auto number = [&](const char* name, float fallback) {
        const auto it = style.find(name);
        return it == style.end() ? fallback : std::atof(it->second.c_str());
    };
    size = std::clamp(size, 1.0f, 256.0f);
    const float scale = stbtt_ScaleForMappingEmToPixels(info, size);
    const int outline = std::clamp(static_cast<int>(number("outline", 0)), 0, 8);
    const float tracking = number("kerning", 0);
    const auto alignment = style.find("align");
    const std::string align = alignment == style.end() ? "left" : alignment->second;
    const auto stroke = style.find("outlinecolor");
    const uint32_t outline_color = stroke == style.end() ? 0 :
        static_cast<uint32_t>(strtoul(stroke->second.c_str(), nullptr, 16));
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &linegap);
    // CJK fonts often keep a much taller hhea clipping box than their
    // typographic em box. Using it as a baseline pushes centered button
    // captions below their scripted top and inflates every line's spacing.
    stbtt_GetFontVMetricsOS2(info, &ascent, &descent, &linegap);
    const float sascent = ascent * scale;
    const float sdescent = descent * scale;
    // spacemiddle separates the reserved ruby row from the main glyph row.
    // It is often negative; omitting rubysize makes adjacent lines overlap.
    const float ruby_row = std::max(0.0, number("rubysize", ruby.empty() ? 0 : size/2));
    const float row_top = number("spacetop", 0) + ruby_row + number("spacemiddle", 0);
    const int baseline = static_cast<int>(std::ceil(sascent + row_top)) + outline;
    const float advance_h = sascent - sdescent + linegap * scale +
        row_top + number("spacebottom", 0);

    // UTF-8 → codepoints → glyphs, then two-pass word layout:
    //   pass 1 measures every glyph's advance;
    //   pass 2 places them into lines, wrapping whenever pen_x would exceed
    //   wrapWidth (>0). Line width = max over the line; total height = #lines.
    const auto decode = [](const std::string& text) {
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        uint32_t cp = c; int len = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = c & 0x1F; len = 2; cp = (cp << 6) | (text[i + 1] & 0x3F); }
        else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) { cp = c & 0x0F; len = 3; cp = (cp << 6) | (text[i + 1] & 0x3F); cp = (cp << 6) | (text[i + 2] & 0x3F); }
        else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) { cp = c & 0x07; len = 4; cp = (cp << 6) | (text[i + 1] & 0x3F); cp = (cp << 6) | (text[i + 2] & 0x3F); cp = (cp << 6) | (text[i + 3] & 0x3F); }
        i += len; cps.push_back(cp);
    }
    return cps;
    };
    auto cps=decode(text);
    const size_t base_count=cps.size();
    std::vector<int> glyphs;
    std::vector<int> advances;
    glyphs.reserve(cps.size()); advances.reserve(cps.size());
    for (uint32_t cp : cps) {
        const int gi = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
        glyphs.push_back(gi);
        advances.push_back(static_cast<int>(std::lround(adv * scale + tracking)));
    }
    if (glyphs.empty()) return false;
    struct RubyGroup { size_t first, last, glyph_first, glyph_last; int base_width, width; };
    std::vector<RubyGroup> ruby_groups;
    const float ruby_size=std::clamp(number("rubysize",size/2),1.0,256.0);
    const float ruby_scale=stbtt_ScaleForMappingEmToPixels(info,ruby_size);
    for(const auto& r:ruby) {
        if(r.start>text.size() || r.length>text.size()-r.start || r.text.empty()) continue;
        const size_t first=decode(text.substr(0,r.start)).size();
        const size_t last=first+decode(text.substr(r.start,r.length)).size();
        if(first>=last || last>base_count) continue;
        if(!ruby_groups.empty() && first<ruby_groups.back().last) continue;
        if(std::find(cps.begin()+first,cps.begin()+last,'\n')!=cps.begin()+last) continue;
        int base_width=0, ruby_width=0;
        for(size_t k=first;k<last;++k) base_width+=advances[k];
        const size_t glyph_first=glyphs.size();
        for(uint32_t cp:decode(r.text)) {
            int gi=stbtt_FindGlyphIndex(info,static_cast<int>(cp)),adv=0,lsb=0;
            stbtt_GetGlyphHMetrics(info,gi,&adv,&lsb);
            cps.push_back(cp); glyphs.push_back(gi);
            advances.push_back(static_cast<int>(std::lround(adv*ruby_scale)));
            ruby_width+=advances.back();
        }
        const int width=std::max(base_width,ruby_width);
        // Ruby and its base occupy one unbreakable block. A longer reading
        // reserves room in the line, so it cannot collide with adjacent text.
        ruby_groups.push_back({first,last,glyph_first,glyphs.size(),base_width,width});
    }

    // layout: (line, pen_x_in_line)
    std::vector<int> lx(glyphs.size()), ly(glyphs.size());
    std::vector<int> line_w(1, 0);
    struct Unit { size_t first,last; int width; const RubyGroup* ruby; };
    std::vector<Unit> units;
    for(size_t k=0;k<base_count;) {
        if(cps[k]=='\r') { lx[k++]=-1;continue; }
        const RubyGroup* group=nullptr;
        for(const auto& r:ruby_groups) if(r.first==k) {group=&r;break;}
        const size_t end=group ? group->last : k+1;
        units.push_back({k,end,group ? group->width : advances[k],group});k=end;
    }
    const bool prohibit=number("prohibit",0)!=0, hung=number("hung",0)!=0;
    int pen = 0, line = 0;
    for(size_t u=0;u<units.size();) {
        const size_t k=units[u].first;
        if(cps[k]=='\n') {lx[k]=-1;pen=0;++line;line_w.push_back(0);++u;continue;}
        size_t end=u+1;int width=units[u].width;
        // Keep an opening bracket with its following text, and a closing
        // mark with the preceding text. Ruby remains one indivisible unit.
        while(prohibit && end<units.size() && cps[units[end].first]!='\n' &&
              (ProhibitLineEnd(cps[units[end-1].last-1]) ||
               (ProhibitLineStart(cps[units[end].first]) && !(hung && HangPunctuation(cps[units[end].first]))))) {
            width+=units[end].width;++end;
        }
        if(pen>0 && wrapWidth>0 && pen+width>wrapWidth && !(hung && HangPunctuation(cps[k]))) {
            pen=0;++line;line_w.push_back(0);
        }
        for(;u<end;++u) {
            const auto& unit=units[u];
            if(const auto* group=unit.ruby) {
              int base_pen=pen+(group->width-group->base_width)/2;
              for(size_t j=group->first;j<group->last;++j) {
                  lx[j]=base_pen; ly[j]=line; base_pen+=advances[j];
              }
              int reading_width=0;
              for(size_t j=group->glyph_first;j<group->glyph_last;++j) reading_width+=advances[j];
              int ruby_pen=pen+(group->width-reading_width)/2;
              for(size_t j=group->glyph_first;j<group->glyph_last;++j) {
                  lx[j]=ruby_pen; ly[j]=line; ruby_pen+=advances[j];
              }
            } else {
              lx[unit.first]=pen;ly[unit.first]=line;
            }
            pen+=unit.width;line_w[line]=std::max(line_w[line],pen);
        }
    }
    const int n_lines = static_cast<int>(line_w.size());
    int tex_w = 0;
    for (int w : line_w) tex_w = w > tex_w ? w : tex_w;
    if (wrapWidth > 0) tex_w = std::max(tex_w, static_cast<int>(std::ceil(wrapWidth)));
    tex_w += outline * 2;
    if (tex_w <= 0 || tex_w > 4096) {
        Log(kLogWarn, "SetText: degenerate width " + std::to_string(tex_w));
        return false;
    }
    const int line_h = std::max(1, static_cast<int>(std::ceil(advance_h)));
    int bottom = line_h * n_lines + outline * 2;
    for (size_t k = 0; k < glyphs.size(); ++k) {
        if (lx[k] < 0) continue;
        int x0, y0, x1, y1;
        const float gs=k<base_count ? scale : ruby_scale;
        stbtt_GetGlyphBitmapBox(info, glyphs[k], gs, gs, &x0, &y0, &x1, &y1);
        bottom = std::max(bottom, baseline + ly[k] * line_h + y1 + outline);
    }
    const int tex_h = bottom;
    if (tex_h <= 0 || tex_h > 4096) return false;

    // Each glyph occupies its own padded atlas cell. Overlapping outlines
    // and kerning must not reveal neighbouring letters during a character
    // tween. The complete line layout stays fixed throughout the animation.
    struct Cell { int x, y, w, h; std::vector<uint8_t> pixels; };
    std::vector<Cell> cells;
    std::vector<TextGlyph> positioned;
    const int atlas_w = 1024;
    int atlas_x=1, atlas_y=1, atlas_row=0;
    for (size_t k=0;k<glyphs.size();++k) {
        if (lx[k]<0) continue;
        int gw=0,gh=0,xoff=0,yoff=0;
        const float gs=k<base_count ? scale : ruby_scale;
        uint8_t* bmp=stbtt_GetGlyphBitmap(info,gs,gs,glyphs[k],&gw,&gh,&xoff,&yoff);
        const int cw=gw+2*outline, ch=gh+2*outline;
        if (atlas_x+cw+1>atlas_w) { atlas_x=1; atlas_y+=atlas_row+2; atlas_row=0; }
        if (cw+2>atlas_w || atlas_y+ch+1>4096) { stbtt_FreeBitmap(bmp,nullptr); return false; }
        std::vector<uint8_t> cov(static_cast<size_t>(cw)*ch,0), pixels(cov.size()*4,0);
        if (bmp) for(int y=0;y<gh;++y) for(int x=0;x<gw;++x)
            cov[static_cast<size_t>(y+outline)*cw+x+outline]=bmp[y*gw+x];
        stbtt_FreeBitmap(bmp,nullptr);
        for(int y=0;y<ch;++y) for(int x=0;x<cw;++x) {
            const size_t off=static_cast<size_t>(y)*cw+x;
            const float fill=cov[off]/255.f;
            uint8_t border=0;
            if(outline) for(int oy=-outline;oy<=outline;++oy) for(int ox=-outline;ox<=outline;++ox) {
                const int xx=x+ox, yy=y+oy;
                if(xx>=0 && xx<cw && yy>=0 && yy<ch && ox*ox+oy*oy<=outline*outline)
                    border=std::max(border,cov[static_cast<size_t>(yy)*cw+xx]);
            }
            const float edge=border/255.f*(1-fill), alpha=fill+edge;
            if(alpha<=0) continue;
            for(int c=0;c<3;++c) {
                const int shift=(2-c)*8;
                pixels[off*4+c]=static_cast<uint8_t>((((color>>shift)&255)*fill+
                    ((outline_color>>shift)&255)*edge)/alpha);
            }
            pixels[off*4+3]=static_cast<uint8_t>(std::lround(alpha*255));
        }
        const int free_width=tex_w-2*outline-line_w[ly[k]];
        const int shift=align=="center" ? free_width/2 : (align=="right" ? free_width : 0);
        TextGlyph g;
        g.order=k;
        for(const auto& r:ruby_groups) if(k>=r.glyph_first && k<r.glyph_last) g.order=r.first;
        const float glyph_baseline=k<base_count ? baseline : outline+number("spacetop",0)+ascent*ruby_scale;
        g.x=lx[k]+xoff+shift; g.y=glyph_baseline+ly[k]*line_h+yoff-outline;
        g.w=cw; g.h=ch;
        g.u0=float(atlas_x)/atlas_w; g.u1=float(atlas_x+cw)/atlas_w;
        g.v0=atlas_y; g.v1=atlas_y+ch; // normalize after the final atlas height is known
        positioned.push_back(g);
        cells.push_back({atlas_x,atlas_y,cw,ch,std::move(pixels)});
        atlas_x+=cw+2; atlas_row=std::max(atlas_row,ch);
    }
    const int atlas_h=std::max(1,atlas_y+atlas_row+1);
    std::vector<uint8_t> rgba(static_cast<size_t>(atlas_w)*atlas_h*4,0);
    for(const auto& cell:cells) for(int y=0;y<cell.h;++y)
        std::copy_n(cell.pixels.data()+static_cast<size_t>(y)*cell.w*4,cell.w*4,
                    rgba.data()+(static_cast<size_t>(cell.y+y)*atlas_w+cell.x)*4);
    for(auto& g:positioned) { g.v0/=atlas_h; g.v1/=atlas_h; }
    const uint32_t tex=CreateTexture(rgba.data(),atlas_w,atlas_h);

    // upsert layer; message-layer default position = bottom-left
    for (auto &l : layers_) {
        if (l.id == id) {
            if (l.texture) glDeleteTextures(1, &l.texture);
            l.texture = tex;
            l.tex_w = atlas_w; l.tex_h = atlas_h;
            SetGlyphTimes(l, positioned, text);
            // The placeholder layer may carry a degenerate (0-sized) rect
            // from its ghost creation — the new raster defines the display
            // size, otherwise the quad collapses and nothing is drawn.
            l.w = (float)tex_w;
            l.h = (float)tex_h;
            l.content_x = number("left", 0);
            l.content_y = number("top", 0);
            Log(kLogInfo, "SetText: replaced " + id + " " +
                              std::to_string(tex_w) + "x" + std::to_string(tex_h));
            return true;
        }
    }
    Layer l;
    l.id = id;
    l.texture = tex;
    l.tex_w = atlas_w; l.tex_h = atlas_h;
    SetGlyphTimes(l, positioned, text);
    l.w = (float)tex_w; l.h = (float)tex_h;
    l.content_x = number("left", 0);
    l.content_y = number("top", 0);
    l.z = 100; // above scene layers
    layers_.push_back(l);
    Log(kLogInfo, "SetText: " + id + " " + std::to_string(tex_w) + "x" +
                      std::to_string(tex_h) + " '" + text.substr(0, 24) + "'");
    return true;
}

void Compositor::SetProps(const std::string &id,
                          const std::map<std::string, std::string> &attrs) {
    ++revision_;
    if(auto mask=attrs.find("intermediate_render_mask");mask!=attrs.end())LoadMask(mask->second);
    for (auto &l : layers_) {
        if (l.id != id) continue;
        l.effect.Set(attrs);
        if (const auto zoom=attrs.find("zoom"); zoom!=attrs.end())
            l.sx=l.sy=ToF(zoom->second,100)/100.f;
        for (const auto &kv : attrs) {
            if (kv.first == "x" || kv.first == "left") { l.x = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "y" || kv.first == "top") { l.y = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "w") l.w = std::stof(kv.second);
            else if (kv.first == "h") l.h = std::stof(kv.second);
            else if (kv.first == "alpha") {
                const float a = std::stof(kv.second);
                l.alpha = std::clamp(a / 255.0f, 0.0f, 1.0f);   // script uses 0-255
            }
            else if (kv.first == "rotate") l.rotate = ToF(kv.second);
            else if (kv.first == "reversex") l.reverse_x = ToF(kv.second)!=0;
            else if (kv.first == "reversey") l.reverse_y = ToF(kv.second)!=0;
            else if (kv.first == "anchorx") l.ax = std::stof(kv.second);
            else if (kv.first == "xscale") { const float v = std::stof(kv.second); l.sx = v / 100.0f; }
            else if (kv.first == "yscale") { const float v = std::stof(kv.second); l.sy = v / 100.0f; }
            else if (kv.first == "ownpos") l.own_pos = true;
            else if (kv.first == "anchory") l.ay = std::stof(kv.second);
            else if (kv.first == "visible") l.visible = (kv.second != "0");
            else if (kv.first == "z") l.z = std::stoi(kv.second);
            else if (kv.first == "draggable") l.draggable = (kv.second != "0");
            else if (kv.first == "dragarea") {
                // {left, top, right, bottom} — offsets in stage units
                if (std::sscanf(kv.second.c_str(), "%f,%f,%f,%f",
                                &l.drag_l, &l.drag_t, &l.drag_r, &l.drag_b) == 4) {
                    l.has_dragarea = true;
                }
            }
            else if (kv.first == "clip") {
                // "x,y,w,h" → normalized UV crop
                int cx = 0, cy = 0, cw = 0, chh = 0;
                if (std::sscanf(kv.second.c_str(), "%d,%d,%d,%d", &cx, &cy, &cw, &chh) == 4 &&
                    l.effect.intermediate==0 && l.tex_w > 0 && l.tex_h > 0 && cw > 0 && chh > 0) {
                    l.u0 = (float)cx / l.tex_w;      l.v0 = (float)cy / l.tex_h;
                    l.u1 = (float)(cx + cw) / l.tex_w; l.v1 = (float)(cy + chh) / l.tex_h;
                    l.w = (float)cw;   // clip defines the displayed sub-image size
                    l.h = (float)chh;
                }
            }
        }
        l.z = 0;   // leading id number is the authoritative z
        return;
    }
    // lyprop may target a pure group layer never created by lyc — materialize
    // it as a texture-less holder so children inherit its transform.
    Layer g;
    g.id = id;
    g.visible = true;
    g.z = 0;
    layers_.push_back(g);
    SetProps(id, attrs);
}

void Compositor::DeleteLayer(const std::string &id) {
    ++revision_;
    // group delete: an id names the whole subtree (artemis groups layers as
    // "500.d.1.0", "500.z.*", ...; scripts delete the parent "500" only).
    const std::string prefix = id + ".";
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (it->id == id ||
            it->id.compare(0, prefix.size(), prefix) == 0) {
            if (it->texture) glDeleteTextures(1, &it->texture);
            it = layers_.erase(it);
        } else {
            ++it;
        }
    }
}

void Compositor::ReleaseGl() {
    shaders_.ReleaseGl();
    for(auto& mask:masks_)if(mask.second.texture)glDeleteTextures(1,&mask.second.texture);
    masks_.clear();
    ++revision_;
    for (auto &l : layers_) {
        if (l.texture) glDeleteTextures(1, &l.texture);
    }
    layers_.clear();
    tweens_.clear();
    tween_set_.clear(); collecting_tweens_ = false;
    if (trans_rule_tex_) { glDeleteTextures(1, &trans_rule_tex_); trans_rule_tex_ = 0; }
    if (last_frame_tex_) { glDeleteTextures(1, &last_frame_tex_); last_frame_tex_ = 0; }
    if (scene_fbo_) { glDeleteFramebuffers(1, &scene_fbo_); scene_fbo_ = 0; }
    if (scene_tex_) { glDeleteTextures(1, &scene_tex_); scene_tex_ = 0; }
    if (prog_.program) { glDeleteProgram(prog_.program); prog_.program = 0; }
    if (tprog_.program) { glDeleteProgram(tprog_.program); tprog_.program = 0; }
    trans_active_ = false;
    trans_have_frame_ = false;
    gl_ready_ = false;
}

void Compositor::Shutdown() {
    ReleaseGl();
    delete static_cast<stbtt_fontinfo *>(font_info_);
    font_info_ = nullptr;
    font_data_.clear();
    font_path_.clear();
    font_ready_ = false;
    present_cb_ = nullptr;
}

bool Compositor::Snapshot(SnapshotImage& output) const {
    if(!gl_ready_ || !scene_fbo_ || stage_w_<1 || stage_h_<1 || uint64_t(stage_w_)*stage_h_>16777216)return false;
    SnapshotImage image;image.width=stage_w_;image.height=stage_h_;image.rgba.resize(size_t(stage_w_)*stage_h_*4);
    GLint framebuffer=0,alignment=0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);glGetIntegerv(GL_PACK_ALIGNMENT,&alignment);
    glBindFramebuffer(GL_FRAMEBUFFER,scene_fbo_);glPixelStorei(GL_PACK_ALIGNMENT,1);
    glReadPixels(0,0,stage_w_,stage_h_,GL_RGBA,GL_UNSIGNED_BYTE,image.rgba.data());
    const auto error=glGetError();glPixelStorei(GL_PACK_ALIGNMENT,alignment);glBindFramebuffer(GL_FRAMEBUFFER,framebuffer);
    if(error!=GL_NO_ERROR)return false;
    const size_t stride=size_t(stage_w_)*4;
    for(int y=0;y<stage_h_/2;++y)
        std::swap_ranges(image.rgba.begin()+y*stride,image.rgba.begin()+(y+1)*stride,image.rgba.begin()+(stage_h_-1-y)*stride);
    output=std::move(image);return true;
}

void Compositor::Draw() {
    if (!gl_ready_) return;

    GLint target = 0, viewport[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &target);
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (!scene_fbo_) {
        glGenTextures(1, &scene_tex_);
        glBindTexture(GL_TEXTURE_2D, scene_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, stage_w_, stage_h_, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1, &scene_fbo_);
        glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, scene_tex_, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            Log(kLogError, "compositor: scene framebuffer incomplete");
            glDeleteFramebuffers(1, &scene_fbo_); scene_fbo_ = 0;
            glDeleteTextures(1, &scene_tex_); scene_tex_ = 0;
            glBindFramebuffer(GL_FRAMEBUFFER, target);
            return;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, scene_fbo_);
    glViewport(0, 0, stage_w_, stage_h_);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    // sort by z (stable: preserve insertion order for equal z)
    std::vector<const Layer *> sorted;
    for (const auto &l : layers_) sorted.push_back(&l);
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Layer *a, const Layer *b) {
                         const int c = ZCmp(a->id, b->id);
                         if (c != 0) return c < 0;   // ascending: lower z first
                         return SectionCount(a->id) < SectionCount(b->id);
                     });

    glUseProgram(prog_.program);
    glDisableVertexAttribArray(prog_.a_opacity);
    glVertexAttrib1f(prog_.a_opacity,1);
    glUniform2f(prog_.u_screen, (float)stage_w_, (float)stage_h_);
    glUniform1i(prog_.u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

    auto draw_leaf=[&](const Layer* l,float inherited,bool top_down) {
        const auto transform=EffectiveTransform(*l);
        const float ea=transform.alpha/inherited;
        if (!transform.visible || !l->texture || ea<=0) return;
        glUseProgram(prog_.program);
        glUniform1f(prog_.u_top_down,top_down?1:0);
        glUniform2f(prog_.u_screen,float(stage_w_),float(stage_h_));
        glUniform1i(prog_.u_tex,0);glActiveTexture(GL_TEXTURE0);
        glDisableVertexAttribArray(prog_.a_opacity);glVertexAttrib1f(prog_.a_opacity,1);
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_SRC_ALPHA,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
        glBindTexture(GL_TEXTURE_2D, l->texture);
        if (!l->glyphs.empty()) {
            std::vector<float> vertices;
            vertices.reserve(l->glyphs.size()*30);
            for (const auto& g:l->glyphs) {
                float gx=g.x, gy=g.y, alpha=1;
                for (const auto& tw:l->text_in) {
                    const double elapsed=now_ms_-g.start_ms;
                    const float t=elapsed<0 ? 0 : (tw.time_ms<=0 ? 1 : std::min(1.0,elapsed/tw.time_ms));
                    const float diff=tw.diff*(1-ApplyEase(tw.ease,t));
                    if (tw.param=="alpha") alpha=std::clamp(1+diff/255.f,0.f,1.f);
                    else if(tw.param=="left") gx+=diff;
                    else if(tw.param=="top") gy+=diff;
                }
                if (alpha<=0 || g.w<=0 || g.h<=0) continue;
                const auto p0=transform.Point(gx,gy), p1=transform.Point(gx+g.w,gy);
                const auto p2=transform.Point(gx,gy+g.h), p3=transform.Point(gx+g.w,gy+g.h);
                vertices.insert(vertices.end(),{p0.first,p0.second,g.u0,g.v0,alpha,
                    p1.first,p1.second,g.u1,g.v0,alpha, p2.first,p2.second,g.u0,g.v1,alpha,
                    p2.first,p2.second,g.u0,g.v1,alpha, p1.first,p1.second,g.u1,g.v0,alpha,
                    p3.first,p3.second,g.u1,g.v1,alpha});
            }
            if(!vertices.empty()) {
                glUniform1f(prog_.u_alpha,ea);
                glVertexAttribPointer(prog_.a_pos,2,GL_FLOAT,GL_FALSE,20,vertices.data());
                glEnableVertexAttribArray(prog_.a_pos);
                glVertexAttribPointer(prog_.a_uv,2,GL_FLOAT,GL_FALSE,20,vertices.data()+2);
                glEnableVertexAttribArray(prog_.a_uv);
                glVertexAttribPointer(prog_.a_opacity,1,GL_FLOAT,GL_FALSE,20,vertices.data()+4);
                glEnableVertexAttribArray(prog_.a_opacity);
                glDrawArrays(GL_TRIANGLES,0,vertices.size()/5);
                glDisableVertexAttribArray(prog_.a_opacity);
                glVertexAttrib1f(prog_.a_opacity,1);
            }
            return;
        }
        glUniform1f(prog_.u_alpha, ea);
        const auto p0=transform.Point(0,0), p1=transform.Point(l->w,0);
        const auto p2=transform.Point(0,l->h), p3=transform.Point(l->w,l->h);
        // interleaved: x, y, u, v per vertex (triangle strip)
        float verts[16] = {
            p0.first,p0.second,l->u0,l->v0, p1.first,p1.second,l->u1,l->v0,
            p2.first,p2.second,l->u0,l->v1, p3.first,p3.second,l->u1,l->v1,
        };
        glVertexAttribPointer(prog_.a_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
        glEnableVertexAttribArray(prog_.a_pos);
        glVertexAttribPointer(prog_.a_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
        glEnableVertexAttribArray(prog_.a_uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    };
    std::map<std::string,uint32_t> textures;
    for(const auto& l:layers_)if(l.texture)textures[l.id]=l.texture;
    std::function<void(size_t,size_t,size_t,uint32_t,bool,float)> draw_range;
    draw_range=[&](size_t first,size_t last,size_t depth,uint32_t target,bool top_down,float inherited) {
        for(size_t i=first;i<last;) {
            const auto* l=sorted[i];const auto transform=EffectiveTransform(*l);
            if(!transform.visible || transform.alpha<=0){++i;continue;}
            size_t next=i+1;
            if(l->effect.Active()) {
                const auto prefix=l->id+".";
                while(next<last && sorted[next]->id.compare(0,prefix.size(),prefix)==0)++next;
                const auto intermediate=shaders_.Begin(depth,stage_w_,stage_h_,target,top_down);
                if(intermediate) {
                    draw_leaf(l,transform.alpha,true);
                    draw_range(i+1,next,depth+1,intermediate,true,transform.alpha);
                    LayerCoverage coverage;
                    const float det=transform.a*transform.d-transform.b*transform.c;
                    if(std::isfinite(det) && std::abs(det)>1e-8f) {
                        const float inverse[]={transform.d/det,-transform.b/det,0,-transform.c/det,transform.a/det,0,
                            (transform.c*transform.ty-transform.d*transform.tx)/det,
                            (transform.b*transform.tx-transform.a*transform.ty)/det,1};
                        std::copy_n(inverse,9,coverage.inverse);
                        if(l->effect.intermediate!=0)if(auto clip=l->effect.parameters.find("clip");clip!=l->effect.parameters.end()) {
                            auto* r=coverage.rect;
                            coverage.clip=std::sscanf(clip->second.c_str(),"%f,%f,%f,%f",r,r+1,r+2,r+3)==4 &&
                                std::all_of(r,r+4,[](float v){return std::isfinite(v);}) && r[2]>=0 && r[3]>=0;
                        }
                        if(auto mask=masks_.find(l->effect.mask);mask!=masks_.end()) {
                            coverage.mask=mask->second.texture;coverage.mask_width=mask->second.width;coverage.mask_height=mask->second.height;
                        }
                    }
                    shaders_.End(depth,l->effect,target,top_down,transform.alpha/inherited,textures,coverage);
                    i=next;continue;
                }
            }
            draw_leaf(l,inherited,top_down);++i;
        }
    };
    draw_range(0,sorted.size(),0,scene_fbo_,false,1);
    // Device-side draw ground truth: every 300 frames dump what actually
    // rendered (drawn/total + first samples with effective rects) so layout
    // bugs are observable from logcat alone.
    static int diag_frames = 0;
    if (++diag_frames % 300 == 1) {
        int drawn = 0, ghost = 0;
        std::string sample;
        for (const Layer *l : sorted) {
            float dx, dy, da; bool dv;
            EffectiveRect(*l, &dx, &dy, &da, &dv);
            if (!dv) continue;
            if (!l->texture) { ++ghost; continue; }
            ++drawn;
            if (drawn <= 16)
                sample += " " + l->id + "(" + std::to_string((int)dx) + "," +
                          std::to_string((int)dy) + " " +
                          std::to_string((int)l->w) + "x" +
                          std::to_string((int)l->h) + " z" +
                          std::to_string(l->z) + ")";
        }
        Log(kLogInfo, "draw[" + std::to_string(diag_frames) + "]: drawn=" +
                          std::to_string(drawn) + " ghost=" +
                          std::to_string(ghost) + "/" +
                          std::to_string(sorted.size()) + sample);
    }
    glDisable(GL_BLEND);
    // Fade the previous scene out on top of the new state. The "from"
    // texture is captured once in BeginTransition (pbuffer still holds
    // the last Present). Copying the full stage every Draw was a GPU
    // sync we do not need for idle frames.
    DrawTransitionOverlay();
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    glUseProgram(prog_.program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene_tex_);
    glUniform1f(prog_.u_top_down,0);
    glUniform1f(prog_.u_alpha, 1);
    const float w = float(stage_w_), h = float(stage_h_);
    const float screen[16] = {0,0,0,1, w,0,1,1, 0,h,0,0, w,h,1,0};
    glVertexAttribPointer(prog_.a_pos, 2, GL_FLOAT, GL_FALSE, 16, screen);
    glEnableVertexAttribArray(prog_.a_pos);
    glVertexAttribPointer(prog_.a_uv, 2, GL_FLOAT, GL_FALSE, 16, screen + 2);
    glEnableVertexAttribArray(prog_.a_uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // [flip]: the composed frame is now the presentation candidate
    if (present_cb_) present_cb_();
}

void Compositor::Init(int stage_w, int stage_h) {
    stage_w_ = stage_w;
    stage_h_ = stage_h;
    InitGl();
}

#else // host stubs — no GL; keep layer math (rects / z / hit-test) functional
      // so `artc drive` can trace and click real UI without a GPU.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

bool Compositor::InitGl() { return false; }
// A non-zero fake handle keeps Layer::texture a valid "has pixels" flag for
// HitLayer/DumpRects; no GL object is created on the host.
constexpr uint32_t kHostTexture = 0xFFFFFFFFu;

bool Compositor::SetPixels(const std::string& id, const uint8_t* rgba, int width, int height) {
    if(!rgba || width<=0 || height<=0) return false;
    SetProps(id,{});
    for(auto& l:layers_) if(l.id==id) {
        if(!l.texture || l.w==l.tex_w) l.w=width;
        if(!l.texture || l.h==l.tex_h) l.h=height;
        l.texture=kHostTexture; l.tex_w=width; l.tex_h=height;
        l.glyphs.clear(); l.text.clear(); return true;
    }
    return false;
}
uint32_t Compositor::CreateTexture(const uint8_t *, int, int) { return 0; }
bool Compositor::LoadImage(const std::string &id, const std::string &file) {
    ++revision_;
    if (!packs_) return false;
    static const char *kExtensions[] = {"", ".png", ".jpg", ".jpeg"};
    std::vector<uint8_t> bytes;
    std::string resolved;
    for (const char *ext : kExtensions) {
        resolved = file + ext;
        if (packs_->Read(resolved, bytes)) break;
    }
    if (bytes.empty()) {
        Log(kLogWarn, "lyc: image not found: " + file);
        return false;
    }
    int w = 0, h = 0, channels = 0;
    uint8_t *pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(),
                                            &w, &h, &channels, 4);
    if (!pixels) {
        Log(kLogWarn, "lyc: decode failed: " + file);
        return false;
    }
    stbi_image_free(pixels);
    for (auto &l : layers_) {
        if (l.id == id) {
            l.texture = kHostTexture;
            l.tex_w = w; l.tex_h = h;
            l.content_x = l.content_y = 0;
            l.w = (float)w; l.h = (float)h;
            l.u0 = l.v0 = 0; l.u1 = l.v1 = 1;
            return true;
        }
    }
    Layer l;
    l.id = id;
    l.texture = kHostTexture;
    l.tex_w = w; l.tex_h = h;
    l.w = (float)w; l.h = (float)h;
    l.z = 0;
    layers_.push_back(l);
    return true;
}
void Compositor::SetProps(const std::string &id,
                          const std::map<std::string, std::string> &attrs) {
    ++revision_;
    for (auto &l : layers_) {
        if (l.id != id) continue;
        l.effect.Set(attrs);
        if (const auto zoom=attrs.find("zoom"); zoom!=attrs.end())
            l.sx=l.sy=ToF(zoom->second,100)/100.f;
        for (const auto &kv : attrs) {
            if (kv.first == "x" || kv.first == "left") { l.x = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "y" || kv.first == "top") { l.y = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "w") l.w = std::stof(kv.second);
            else if (kv.first == "h") l.h = std::stof(kv.second);
            else if (kv.first == "alpha") {
                const float a = std::stof(kv.second);
                l.alpha = std::clamp(a / 255.0f, 0.0f, 1.0f);
            }
            else if (kv.first == "rotate") l.rotate = ToF(kv.second);
            else if (kv.first == "reversex") l.reverse_x = ToF(kv.second)!=0;
            else if (kv.first == "reversey") l.reverse_y = ToF(kv.second)!=0;
            else if (kv.first == "anchorx") l.ax = std::stof(kv.second);
            else if (kv.first == "xscale") { const float v = std::stof(kv.second); l.sx = v / 100.0f; }
            else if (kv.first == "yscale") { const float v = std::stof(kv.second); l.sy = v / 100.0f; }
            else if (kv.first == "ownpos") l.own_pos = true;
            else if (kv.first == "anchory") l.ay = std::stof(kv.second);
            else if (kv.first == "visible") l.visible = (kv.second != "0");
            else if (kv.first == "z") l.z = std::stoi(kv.second);
            else if (kv.first == "draggable") l.draggable = (kv.second != "0");
            else if (kv.first == "dragarea") {
                if (std::sscanf(kv.second.c_str(), "%f,%f,%f,%f",
                                &l.drag_l, &l.drag_t, &l.drag_r, &l.drag_b) == 4) {
                    l.has_dragarea = true;
                }
            }
            else if (kv.first == "clip") {
                int cx = 0, cy = 0, cw = 0, chh = 0;
                if (std::sscanf(kv.second.c_str(), "%d,%d,%d,%d", &cx, &cy, &cw, &chh) == 4 &&
                    l.effect.intermediate==0 && l.tex_w > 0 && l.tex_h > 0 && cw > 0 && chh > 0) {
                    l.u0 = (float)cx / l.tex_w;      l.v0 = (float)cy / l.tex_h;
                    l.u1 = (float)(cx + cw) / l.tex_w; l.v1 = (float)(cy + chh) / l.tex_h;
                    l.w = (float)cw;
                    l.h = (float)chh;
                }
            }
        }
        l.z = 0;
        return;
    }
    // texture-less ghost layer for pure transform holders
    Layer g;
    g.id = id;
    g.visible = true;
    g.z = 0;
    layers_.push_back(g);
    SetProps(id, attrs);
}
void Compositor::DeleteLayer(const std::string &id) {
    ++revision_;
    // group delete: an id names the whole subtree (artemis groups layers as
    // "500.d.1.0", "500.z.*", ...; scripts delete the parent "500" only).
    const std::string prefix = id + ".";
    for (auto it = layers_.begin(); it != layers_.end();) {
        if (it->id == id ||
            it->id.compare(0, prefix.size(), prefix) == 0) {
            it = layers_.erase(it);
        } else {
            ++it;
        }
    }
}
bool Compositor::LoadFont(const std::string &file) {
    Log(kLogInfo, "font (host, no raster): " + file);
    return false;
}
bool Compositor::SetText(const std::string &id, const std::string &text,
                         float size, uint32_t color, float wrapWidth,
                         const std::map<std::string, std::string>& style,
                         const std::vector<TextRuby>& ruby) {
    ++revision_;
    if (text.empty()) return false;
    (void)color;   // host mock ignores the baked-in color (no rasterization)
    (void)wrapWidth;
    // fake-texture message layer: width ~ proportional to glyph count
    const int tw = std::max(1, static_cast<int>(size * text.size()));
    const int th = std::max(1, static_cast<int>(size * 1.5f));
    for (auto &l : layers_) {
        if (l.id == id) {
            l.texture = kHostTexture;
            l.tex_w = tw; l.tex_h = th;
            l.w = (float)tw; l.h = (float)th;
            Log(kLogInfo, "SetText: replaced " + id + " " + std::to_string(tw) + "x" +
                              std::to_string(th) + " '" + text.substr(0, 24) + "'");
            return true;
        }
    }
    Layer l;
    l.id = id;
    l.texture = kHostTexture;
    l.tex_w = tw; l.tex_h = th;
    l.w = (float)tw; l.h = (float)th;
    l.x = 40.0f;
    l.y = (float)stage_h_ - (float)th - 60.0f;
    l.z = 100;
    layers_.push_back(l);
    Log(kLogInfo, "SetText: " + id + " " + std::to_string(tw) + "x" +
                      std::to_string(th) + " '" + text.substr(0, 24) + "'");
    return true;
}
void Compositor::Shutdown() { layers_.clear(); present_cb_ = nullptr; }
void Compositor::ReleaseGl() { ++revision_; layers_.clear(); tweens_.clear(); tween_set_.clear(); collecting_tweens_ = false; trans_active_ = false; gl_ready_ = false; }
void Compositor::CaptureFrame() {}
bool Compositor::Snapshot(SnapshotImage&) const {return false;}
void Compositor::DrawTransitionOverlay() {}
bool Compositor::BeginTransition(double now_ms, int time_ms, const std::vector<uint8_t> &,
                                 int, int, int) {
    if (time_ms <= 0) return false;
    trans_start_ms_ = now_ms; trans_time_ms_ = time_ms; trans_active_ = true; ++revision_;
    return true;
}
void Compositor::Draw() {}
void Compositor::Init(int stage_w, int stage_h) {
    stage_w_ = stage_w;
    stage_h_ = stage_h;
}

#endif

} // namespace artc
