// compositor.h — z-ordered layer compositor (GLES2).
//
// The adv framework drives this through graphics tags:
//   e:tag{lyc,    id="bg1", file="image/back/bg_00c.jpg"}  load image
//   e:tag{lyprop, id="bg1", x=0, y=0, w=1280, h=720, alpha=1} set props
//   e:tag{lydel,  id="bg1"}                                   remove
//   e:tag{flip}                                               present
//
// Stage coordinates: WIDTH×HEIGHT from system.ini (e.g. 1280×720), mapped to
// the render surface by an orthographic transform in the shader.
#pragma once
#include "render/layer_shader.h"
#include "render/snapshot_image.h"
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <set>
#include <utility>

namespace artc {

class PackManager;

struct TextGlyph {
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    double start_ms = 0;
    size_t order = 0;
};
struct TextRuby {
    size_t start = 0, length = 0; // UTF-8 byte range in the base text
    std::string text;
};
struct TextTween {
    std::string param;
    double delay_ms = 0, time_ms = 0;
    float diff = 0;
    int ease = 0;
};

struct Layer {
    std::string id;
    LayerEffect effect;
    uint32_t texture = 0;      // GL texture name (0 = no texture)
    int tex_w = 0, tex_h = 0;
    float x = 0, y = 0;        // draw position (anchor applied)
    float content_x = 0, content_y = 0; // text rectangle origin inside the layer
    float w = 0, h = 0;        // display size in stage units (0 = natural size)
    float alpha = 1.0f;
    bool visible = true;
    int z = 0;                 // higher = closer to viewer (ID leading number)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // clip region (normalized UV)
    float ax = 0, ay = 0;      // anchor point within the image
    float sx = 1.0f, sy = 1.0f; // xscale/yscale (lyprop percent / 100), pivot = anchor
    float rotate = 0;          // clockwise degrees in the y-down stage
    bool reverse_x = false, reverse_y = false;
    bool own_pos = false;      // lyprop set an explicit position on this layer
    // [lyprop draggable/dragarea] — the framework's slider pins are
    // draggable within a rect given as {left, top, right, bottom} offsets
    // in stage units (relative to the layer's own origin).
    bool draggable = false;
    float drag_l = 0, drag_t = 0, drag_r = 0, drag_b = 0;
    bool has_dragarea = false;
    std::string text;
    std::vector<TextGlyph> glyphs;
    std::vector<TextTween> text_in;
};

class Compositor {
public:
    void Init(int stage_w, int stage_h);   // stage resolution from system.ini
    void Shutdown();

    // Tag handlers (called from the Lua bridge on the engine thread).
    void SetPackManager(PackManager *packs) { packs_ = packs; }
    void SetSaveDirectory(const std::string& directory) { save_directory_=directory; }
    bool LoadImage(const std::string &id, const std::string &file);
    bool LoadShader(const std::string& id, const std::string& file);
    bool SetPixels(const std::string& id, const uint8_t* rgba, int width, int height);
    // Capture the retained stage without redrawing the save/menu overlays.
    bool Snapshot(SnapshotImage& output) const;
    void SetProps(const std::string &id, const std::map<std::string, std::string> &attrs);
    void DeleteLayer(const std::string &id);

    // ---- text (M2.2 message layer pipeline) ----
    // Load a TTF/OTF from the pack chain (font/xxx.ttf in game data).
    bool LoadFont(const std::string &file);
    bool FontReady() const { return font_ready_; }
    // Rasterize a UTF-8 string into a single RGBA texture and put it on a
    // layer (message-layer style). color = 0xRRGGBB. Returns false when no
    // font is loaded. An empty string clears the existing text texture.
    bool SetText(const std::string &id, const std::string &text, float size,
                 uint32_t color, float wrapWidth = 0,
                 const std::map<std::string, std::string>& style = {},
                 const std::vector<TextRuby>& ruby = {});
    void SetTextTween(const std::string& id, const std::map<std::string, std::string>& attrs);
    double PendingTextMs(double now_ms) const;
    bool FinishText(double now_ms);

    // GL draw (called from the render loop on the engine thread). Draws all
    // visible layers, then invokes the present callback ([flip] semantics).
    void Draw();

    // One-shot per-layer effective-rect tracing (id, x, y, w, h, z). Shared by
    // the Android draw path and the host `artc drive` frame loop so both can
    // diagnose layout without guessing.
    void DumpRects();

    // Present hook: the engine thread binds this to Renderer::Present so a
    // [flip] tag both draws and swaps. Without it, Draw only renders GL.
    void SetPresent(std::function<void()> cb) { present_cb_ = std::move(cb); }

    // Drop GL resources (context is going away on window loss). Layer state is
    // discarded; a re-boot re-creates everything through the tags.
    void ReleaseGl();

    // Topmost visible textured layer whose rect contains the stage point.
    void EffectiveRect(const Layer &l, float *ex, float *ey,
                       float *ea, bool *ev) const;
    // Signed origin/size for axis-aligned transforms; bounding rectangle
    // otherwise. Drawing and hit tests use EffectiveTransform directly.
    void EffectiveRect(const Layer &l, float *ex, float *ey, float *ew,
                       float *eh, float *ea, bool *ev) const;
    struct Transform {
        float a=1, b=0, c=0, d=1, tx=0, ty=0;
        float alpha=1;
        bool visible=true;
        std::pair<float,float> Point(float x, float y) const {
            return {a*x+c*y+tx, b*x+d*y+ty};
        }
    };
    // Local content coordinates -> stage; used by rendering and inverse hit tests.
    Transform EffectiveTransform(const Layer& layer) const;
    // Convert a pointer displacement to the coordinate space of lyprop left/top.
    bool ParentDelta(const std::string& id, float dx, float dy, float* x, float* y) const;
    std::string HitLayer(float x, float y) const;
    // KrKr2-Next: every visible textured layer containing the stage point,
    // topmost first. Lets the input layer pick the frontmost *event-bearing*
    // layer instead of a decorative child drawn over it (choice text over
    // its button image).
    std::vector<std::string> HitLayers(float x, float y) const;

    // [var system="get_layer_info"] — return the layer's stored position
    // (relative offsets, matching draggable semantics) and draggable bounds.
    struct LayerInfo {
        bool found = false;
        float left = 0, top = 0;      // stored relative offsets
        float width = 0, height = 0;  // display size in stage units
        bool draggable = false;
        bool has_dragarea = false;
        float drag_l = 0, drag_t = 0, drag_r = 0, drag_b = 0;
    };
    LayerInfo GetLayerInfo(const std::string &id) const;

    int StageWidth() const { return stage_w_; }
    int StageHeight() const { return stage_h_; }

    // KrKr2-Next: read-only view of the layer table (tests / diagnostics).
    const std::vector<Layer> &Layers() const { return layers_; }
    // KrKr2-Next: one-line summary of the draw list in z order (topmost
    // last): id(x,y wxh a=alpha) for textured visible layers, plus the
    // animation state. Used by the host's periodic diagnostics.
    std::string DescribeDrawList(size_t max_layers) const;

    // KrKr2-Next host hook: monotonically increasing layer-state revision.
    // Bumped by every mutation (LoadImage / SetProps / DeleteLayer / SetText /
    // ReleaseGl) so the bridge can skip the expensive frame readback when
    // nothing changed since the last presented frame.
    uint64_t Revision() const { return revision_; }

    // ---- KrKr2-Next: tweens ([lytween]) and transitions ([trans]) ----
    // Time base: milliseconds on a monotonic clock supplied by the host
    // (LuaEngine hands over its e:now() clock). Tweens animate one layer
    // property (alpha / left / top / xscale / yscale / zoom / w / h) between
    // `from` and `to` over `time` ms after `delay` ms with an easing curve;
    // loop/yoyo count extra traversals (-1 = indefinite). A tweenset queues
    // successive segments of each layer/property, keeping distinct properties
    // parallel (e.g. the two components of a zoom).
    // [lytweendel id] finishes the tweens of a layer and its subtree.
    void AddTween(const std::string &id, const std::map<std::string, std::string> &attrs,
                  double now_ms);
    void BeginTweenSet();
    void EndTweenSet(double now_ms);
    void DeleteTweens(const std::string &id);
    // Advance tweens/transition to `now_ms`; returns true when the picture
    // changed (the caller redraws). Call once per frame before Draw().
    bool Update(double now_ms);
    // Remaining animation time (ms) — what [wt] / the transition wait need.
    double PendingAnimationMs(double now_ms) const;

    // [trans time= rule= vague=] — crossfade from the frame that was on
    // screen when the tag arrived to the current layer state. `rule` is the
    // decoded 8-bit rule image (row-major, stage sized; empty = plain fade),
    // dark pixels transition first, `vague` (0-255) softens the edge.
    bool BeginTransition(double now_ms, int time_ms, const std::vector<uint8_t> &rule,
                         int rule_w, int rule_h, int vague);
    bool TransitionActive() const { return trans_active_; }
    // Remaining transition time (ms). The kernel's wt holds the script for the
    // transition only — unrelated long tweens (a slow background pan started
    // under the trans) keep running while the dialogue continues.
    double TransitionRemainingMs(double now_ms) const {
        if (!trans_active_) return 0;
        const double remain = trans_start_ms_ + trans_time_ms_ - now_ms;
        return remain > 0 ? remain : 0;
    }

private:
    uint64_t revision_ = 0;
    struct GlProgram {
        uint32_t program = 0;
        int a_pos = -1, a_uv = -1, a_opacity = -1;
        int u_screen = -1, u_tex = -1, u_alpha = -1, u_top_down = -1;
    };
    struct TransProgram {
        uint32_t program = 0;
        int a_pos = -1, a_uv = -1;
        int u_screen = -1, u_tex = -1, u_rule = -1, u_t = -1, u_vague = -1, u_use_rule = -1;
    };
    struct Tween {
        std::string id;
        std::string param;
        float from = 0, to = 0;
        double start_ms = 0;     // when the delay started
        double delay_ms = 0, time_ms = 0;
        int ease = 0;
        bool from_current = true; // resolve `from` from the layer on first update
        bool started = false;
        int repeat = 0;          // extra traversals; -1 repeats indefinitely
        bool yoyo = false;       // alternate traversal direction
        double Duration() const;
        float FinalValue() const;
    };

    bool InitGl();
    bool ContainsPoint(const Layer& layer, float x, float y) const;
    uint32_t CreateTexture(const uint8_t *pixels, int w, int h);
    void DrawTransitionOverlay();
    void CaptureFrame();
    static bool ApplyParam(Layer &l, const std::string &param, float value);
    static bool ReadParam(const Layer &l, const std::string &param, float *value);
    void QueueTween(Tween tw, bool replace);
    static double TextEnd(const Layer& layer);
    void SetGlyphTimes(Layer& layer, std::vector<TextGlyph>& glyphs, const std::string& text);

    int stage_w_ = 1280, stage_h_ = 720;
    LayerShaders shaders_;
    std::string save_directory_;
    struct MaskTexture {uint32_t texture=0;int width=0,height=0;};
    std::map<std::string,MaskTexture> masks_;
    void LoadMask(const std::string& file);
    GlProgram prog_{};
    TransProgram tprog_{};
    bool gl_ready_ = false;

    std::vector<Tween> tweens_;
    bool collecting_tweens_ = false;
    std::vector<Tween> tween_set_;
    double now_ms_ = 0;
    // transition state
    bool trans_active_ = false;
    double trans_start_ms_ = 0, trans_time_ms_ = 0;
    float trans_vague_ = 0;
    uint32_t trans_rule_tex_ = 0;
    uint32_t last_frame_tex_ = 0;   // copy of the last composited frame
    uint32_t scene_fbo_ = 0, scene_tex_ = 0; // retained scene, independent of swap buffers
    bool trans_have_frame_ = false;
    std::vector<Layer> layers_;   // draw order = vector order
    PackManager *packs_ = nullptr;
    std::function<void()> present_cb_;
    std::set<std::string> dumped_;   // one-shot rect dump

    // font state (stbtt_fontinfo is heap-pimpl'd to keep this header lean)
    std::vector<uint8_t> font_data_;
    std::string font_path_;
    void *font_info_ = nullptr;
    bool font_ready_ = false;
};

} // namespace artc
