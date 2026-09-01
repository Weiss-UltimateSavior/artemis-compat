#include "render/compositor.h"
#include "pack/pack_manager.h"
#include "pack/pf8_reader.h"
#include "log/logger.h"

#if defined(__ANDROID__)
#include <GLES2/gl2.h>
#define STB_IMAGE_IMPLEMENTATION
#include "render/stb_image.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "render/stb_truetype.h"
#endif

#include <algorithm>
#include <set>
#include <cstdio>

namespace artc {

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
    // Position semantics (validated against the real engine on device):
    //  * a layer with its OWN explicit position (own_pos) renders at its
    //    stored left/top ABSOLUTE stage coordinates (the message window, the
    //    CG containers, the top-bar dock and the title buttons all place
    //    themselves directly);
    //  * a layer WITHOUT an explicit position (button art "500.d.1.0",
    //    message text "1.80.mw.adv_adv") inherits the nearest positioned
    //    ancestor's final position exactly — coordinates do NOT double-stack
    //    along the chain;
    //  * anchor(x/y) is a scale/rotation pivot only (with scale=1/rot=0 it
    //    does not shift the draw origin); anchored children inherit the
    //    ancestor's final position directly;
    //  * a DRAGGABLE slider pin stores a RELATIVE offset in its track; it is
    //    added to the nearest positioned ancestor's absolute position.
    float a = l.alpha;
    bool vis = l.visible;
    float x = 0, y = 0;
    float anc_x = 0, anc_y = 0;
    bool anchored = false;
    std::string id = l.id;
    size_t dot;
    while ((dot = id.rfind('.')) != std::string::npos) {
        id = id.substr(0, dot);
        for (const auto &p : layers_) {
            if (p.id != id) continue;
            a *= p.alpha;
            vis = vis && p.visible;
            if (!anchored && p.own_pos) {
                float ax2, ay2, aa; bool av;
                EffectiveRect(p, &ax2, &ay2, &aa, &av);
                anc_x = ax2; anc_y = ay2; anchored = true;
            }
            break;
        }
    }
    if (l.own_pos && l.draggable) {
        x = anc_x + l.x - l.ax; y = anc_y + l.y - l.ay;
    } else if (l.own_pos) {
        x = l.x - l.ax; y = l.y - l.ay;   // absolute stage position
    } else if (anchored) {
        x = anc_x; y = anc_y;             // inherit ancestor's final position
    } else {
        x = l.x - l.ax; y = l.y - l.ay;
    }
    *ex = x; *ey = y; *ea = a; *ev = vis;
}

std::string Compositor::HitLayer(float x, float y) const {
    const Layer *best = nullptr;
    for (const auto &l : layers_) {
        float ex, ey, ea; bool ev;
        EffectiveRect(l, &ex, &ey, &ea, &ev);
        if (!ev || !l.texture) continue;
        if (x < ex || y < ey || x >= ex + l.w || y >= ey + l.h) continue;
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

#if defined(__ANDROID__)

namespace {
const char *kVs = R"(attribute vec2 a_pos;
attribute vec2 a_uv;
uniform vec2 u_screen;
varying vec2 v_uv;
void main() {
    vec2 clip = vec2(a_pos.x / u_screen.x * 2.0 - 1.0,
                     1.0 - a_pos.y / u_screen.y * 2.0);
    gl_Position = vec4(clip, 0.0, 1.0);
    v_uv = a_uv;
})";

const char *kFs = R"(precision mediump float;
varying vec2 v_uv;
uniform sampler2D u_tex;
uniform float u_alpha;
void main() {
    vec4 c = texture2D(u_tex, v_uv);
    gl_FragColor = vec4(c.rgb, c.a * u_alpha);
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
    glLinkProgram(prog_.program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    prog_.a_pos = glGetAttribLocation(prog_.program, "a_pos");
    prog_.a_uv = glGetAttribLocation(prog_.program, "a_uv");
    prog_.u_screen = glGetUniformLocation(prog_.program, "u_screen");
    prog_.u_tex = glGetUniformLocation(prog_.program, "u_tex");
    prog_.u_alpha = glGetUniformLocation(prog_.program, "u_alpha");
    gl_ready_ = true;
    return true;
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
    if (!gl_ready_ || !packs_) return false;
    // Scripts usually omit extensions (:bg/black); the real engine matches
    // packed assets by trying the common image extensions.
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
            if (l.w == 0) { l.w = (float)w; l.h = (float)h; }
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

bool Compositor::LoadFont(const std::string &file) {
    if (!gl_ready_ || !packs_) return false;
    if (!packs_->Read(file, font_data_)) {
        Log(kLogWarn, "font not found in packs: " + file);
        return false;
    }
    auto *info = new stbtt_fontinfo;
    const int offset = stbtt_GetFontOffsetForIndex(font_data_.data(), 0);
    if (offset < 0 || !stbtt_InitFont(info, font_data_.data(), offset)) {
        Log(kLogError, "font init failed: " + file);
        delete info;
        return false;
    }
    delete static_cast<stbtt_fontinfo *>(font_info_);
    font_info_ = info;
    font_ready_ = true;
    Log(kLogInfo, "font loaded: " + file + " (" + std::to_string(font_data_.size()) + " B)");
    return true;
}

bool Compositor::SetText(const std::string &id, const std::string &text,
                         float size, uint32_t color, float wrapWidth) {
    if (!font_ready_ || text.empty()) return false;
    auto *info = static_cast<stbtt_fontinfo *>(font_info_);
    const float scale = stbtt_ScaleForPixelHeight(info, size);
    int ascent = 0, descent = 0, linegap = 0;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &linegap);
    const float sascent = ascent * scale;
    const float sdescent = descent * scale;
    const int baseline = static_cast<int>(sascent);
    const float advance_h = sascent - sdescent + 3;   // line pitch

    // UTF-8 → codepoints → glyphs, then two-pass word layout:
    //   pass 1 measures every glyph's advance;
    //   pass 2 places them into lines, wrapping whenever pen_x would exceed
    //   wrapWidth (>0). Line width = max over the line; total height = #lines.
    std::vector<uint32_t> cps;
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = text[i];
        uint32_t cp = c; int len = 1;
        if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) { cp = c & 0x1F; len = 2; cp = (cp << 6) | (text[i + 1] & 0x3F); }
        else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) { cp = c & 0x0F; len = 3; cp = (cp << 6) | (text[i + 1] & 0x3F); cp = (cp << 6) | (text[i + 2] & 0x3F); }
        else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) { cp = c & 0x07; len = 4; cp = (cp << 6) | (text[i + 1] & 0x3F); cp = (cp << 6) | (text[i + 2] & 0x3F); cp = (cp << 6) | (text[i + 3] & 0x3F); }
        i += len; cps.push_back(cp);
    }
    std::vector<int> glyphs;
    std::vector<int> advances;
    glyphs.reserve(cps.size()); advances.reserve(cps.size());
    for (uint32_t cp : cps) {
        const int gi = stbtt_FindGlyphIndex(info, static_cast<int>(cp));
        int adv = 0, lsb = 0;
        stbtt_GetGlyphHMetrics(info, gi, &adv, &lsb);
        glyphs.push_back(gi);
        advances.push_back(static_cast<int>(adv * scale) + 1);
    }
    if (glyphs.empty()) return false;

    // layout: (line, pen_x_in_line)
    std::vector<int> lx(glyphs.size()), ly(glyphs.size());
    std::vector<int> line_w(1, 0);
    int pen = 0, line = 0;
    for (size_t k = 0; k < glyphs.size(); ++k) {
        if (pen > 0 && wrapWidth > 0 && pen + advances[k] > wrapWidth) {
            pen = 0; ++line; line_w.push_back(0);
        }
        lx[k] = pen; ly[k] = line;
        pen += advances[k];
        if (line_w[line] < pen) line_w[line] = pen;
    }
    const int n_lines = static_cast<int>(line_w.size());
    int tex_w = 0;
    for (int w : line_w) tex_w = w > tex_w ? w : tex_w;
    if (tex_w <= 0 || tex_w > 4096) {
        Log(kLogWarn, "SetText: degenerate width " + std::to_string(tex_w));
        return false;
    }
    const int line_h = static_cast<int>(advance_h);
    const int tex_h = line_h * n_lines + 2;

    std::vector<uint8_t> rgba(static_cast<size_t>(tex_w) * tex_h * 4, 0);
    const uint8_t r = (color >> 16) & 0xFF, g = (color >> 8) & 0xFF, b = color & 0xFF;
    for (size_t k = 0; k < glyphs.size(); ++k) {
        const int gi = glyphs[k];
        int gw = 0, gh = 0, xoff = 0, yoff = 0;
        uint8_t *bmp = stbtt_GetGlyphBitmap(info, scale, scale, gi, &gw, &gh, &xoff, &yoff);
        if (bmp) {
            const int dx0 = lx[k] + xoff;
            const int dy0 = baseline + ly[k] * line_h + yoff;
            for (int row = 0; row < gh; ++row) {
                const int dy = dy0 + row;
                if (dy < 0 || dy >= tex_h) continue;
                for (int col = 0; col < gw; ++col) {
                    const int dx = dx0 + col;
                    if (dx < 0 || dx >= tex_w) continue;
                    const uint8_t cov = bmp[row * gw + col];
                    uint8_t *px = &rgba[(static_cast<size_t>(dy) * tex_w + dx) * 4];
                    px[0] = r; px[1] = g; px[2] = b; px[3] = cov;
                }
            }
            stbtt_FreeBitmap(bmp, nullptr);
        }
    }

    const uint32_t tex = CreateTexture(rgba.data(), tex_w, tex_h);

    // upsert layer; message-layer default position = bottom-left
    for (auto &l : layers_) {
        if (l.id == id) {
            if (l.texture) glDeleteTextures(1, &l.texture);
            l.texture = tex;
            l.tex_w = tex_w; l.tex_h = tex_h;
            // The placeholder layer may carry a degenerate (0-sized) rect
            // from its ghost creation — the new raster defines the display
            // size, otherwise the quad collapses and nothing is drawn.
            l.w = (float)tex_w;
            l.h = (float)tex_h;
            Log(kLogInfo, "SetText: replaced " + id + " " +
                              std::to_string(tex_w) + "x" + std::to_string(tex_h));
            return true;
        }
    }
    Layer l;
    l.id = id;
    l.texture = tex;
    l.tex_w = tex_w; l.tex_h = tex_h;
    l.w = (float)tex_w; l.h = (float)tex_h;
    l.x = 40.0f;
    l.y = (float)stage_h_ - (float)tex_h - 60.0f;
    l.z = 100; // above scene layers
    layers_.push_back(l);
    Log(kLogInfo, "SetText: " + id + " " + std::to_string(tex_w) + "x" +
                      std::to_string(tex_h) + " '" + text.substr(0, 24) + "'");
    return true;
}

void Compositor::SetProps(const std::string &id,
                          const std::map<std::string, std::string> &attrs) {
    for (auto &l : layers_) {
        if (l.id != id) continue;
        for (const auto &kv : attrs) {
            if (kv.first == "x" || kv.first == "left") { l.x = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "y" || kv.first == "top") { l.y = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "w") l.w = std::stof(kv.second);
            else if (kv.first == "h") l.h = std::stof(kv.second);
            else if (kv.first == "alpha") {
                const float a = std::stof(kv.second);
                l.alpha = a > 1.0f ? a / 255.0f : a;   // script uses 0-255
            }
            else if (kv.first == "anchorx") l.ax = std::stof(kv.second);
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
                    l.tex_w > 0 && l.tex_h > 0 && cw > 0 && chh > 0) {
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
    for (auto &l : layers_) {
        if (l.texture) glDeleteTextures(1, &l.texture);
    }
    layers_.clear();
    gl_ready_ = false;
}

void Compositor::Shutdown() {
    ReleaseGl();
    delete static_cast<stbtt_fontinfo *>(font_info_);
    font_info_ = nullptr;
    font_data_.clear();
    font_ready_ = false;
    present_cb_ = nullptr;
}

void Compositor::Draw() {
    if (!gl_ready_) return;

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
    glUniform2f(prog_.u_screen, (float)stage_w_, (float)stage_h_);
    glUniform1i(prog_.u_tex, 0);
    glActiveTexture(GL_TEXTURE0);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    for (const Layer *l : sorted) {
        float ex, ey, ea; bool ev;
        EffectiveRect(*l, &ex, &ey, &ea, &ev);
        if (!ev || !l->texture) continue;
        glBindTexture(GL_TEXTURE_2D, l->texture);
        glUniform1f(prog_.u_alpha, ea);
        float x0 = ex, y0 = ey, x1 = ex + l->w, y1 = ey + l->h;
        // interleaved: x, y, u, v per vertex (triangle strip)
        float verts[16] = {
            x0, y0, l->u0, l->v0,   x1, y0, l->u1, l->v0,
            x0, y1, l->u0, l->v1,   x1, y1, l->u1, l->v1,
        };
        glVertexAttribPointer(prog_.a_pos, 2, GL_FLOAT, GL_FALSE, 16, verts);
        glEnableVertexAttribArray(prog_.a_pos);
        glVertexAttribPointer(prog_.a_uv, 2, GL_FLOAT, GL_FALSE, 16, verts + 2);
        glEnableVertexAttribArray(prog_.a_uv);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
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

uint32_t Compositor::CreateTexture(const uint8_t *, int, int) { return 0; }
bool Compositor::LoadImage(const std::string &id, const std::string &file) {
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
            if (l.w == 0) { l.w = (float)w; l.h = (float)h; }
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
    for (auto &l : layers_) {
        if (l.id != id) continue;
        for (const auto &kv : attrs) {
            if (kv.first == "x" || kv.first == "left") { l.x = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "y" || kv.first == "top") { l.y = std::stof(kv.second); l.own_pos = true; }
            else if (kv.first == "w") l.w = std::stof(kv.second);
            else if (kv.first == "h") l.h = std::stof(kv.second);
            else if (kv.first == "alpha") {
                const float a = std::stof(kv.second);
                l.alpha = a > 1.0f ? a / 255.0f : a;
            }
            else if (kv.first == "anchorx") l.ax = std::stof(kv.second);
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
                    l.tex_w > 0 && l.tex_h > 0 && cw > 0 && chh > 0) {
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
                         float size, uint32_t color, float wrapWidth) {
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
void Compositor::ReleaseGl() { layers_.clear(); gl_ready_ = false; }
void Compositor::Draw() {}
void Compositor::Init(int stage_w, int stage_h) {
    stage_w_ = stage_w;
    stage_h_ = stage_h;
}

#endif

} // namespace artc
