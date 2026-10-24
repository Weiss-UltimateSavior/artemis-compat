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
#include "render/glyph_atlas.h"
#include "render/layer_shader.h"
#include "save/native_save.h"
#include "util/snapshot_image.h"
#include "util/text.h"
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <set>
#include <utility>

namespace artc {

class PackManager;

struct TextGlyph {
    float x = 0, y = 0, w = 0, h = 0;
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    uint32_t tex = 0;   // glyph-atlas page texture (persistent cache)
    double start_ms = 0;
    size_t order = 0;
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
    bool message_overlay = false; // chgmsg layered=0: outside the scene's ID order
    int z = 0;                 // higher = closer to viewer (ID leading number)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // clip region (normalized UV)
    float ax = 0, ay = 0;      // anchor point within the image
    float sx = 1.0f, sy = 1.0f; // xscale/yscale (lyprop percent / 100), pivot = anchor
    float rotate = 0;          // clockwise degrees in the y-down stage
    bool reverse_x = false, reverse_y = false;
    // Opt-in local linear matrix; x/y remain the only stored translation.
    bool affine = false;
    float affine_a = 1, affine_b = 0, affine_c = 0, affine_d = 1;
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
    // End-of-text pen in texture space (where the next character would go),
    // recorded by SetText. [glyph homing=1] parks the click-wait glyph here.
    float text_pen_x = 0, text_pen_y = 0;
    // Optional warped triangle list (interleaved x,y,u,v in layer-local
    // pixels); when non-empty the layer is drawn as triangles instead of a quad
    // (E-mote per-icon mesh warp).
    std::vector<float> mesh;
    // Semantic sources retained for ARCV v2 reconstruction (never GL handles).
    std::string source_file;
    float text_size = 0;
    uint32_t text_color = 0;
    float text_wrap_width = 0;
    std::map<std::string, std::string> text_style;
    std::vector<TextRuby> text_ruby;
};

class Compositor {
public:
    void Init(int stage_w, int stage_h);   // stage resolution from system.ini
    void Shutdown();

    // Tag handlers (called from the Lua bridge on the engine thread).
    void SetPackManager(PackManager *packs) { packs_ = packs; }
    void SetSaveDirectory(const std::string& directory) { save_directory_=directory; }
    // [prohibit]/[wordparts]/[indent]: configure CJK line-break rules. Empty
    // sets fall back to the documented defaults.
    void SetProhibitRules(const std::string& head, const std::string& foot);
    void SetWordparts(const std::string& parts);
    void SetIndentRules(const std::string& pair, int range, bool nest);
    bool LoadImage(const std::string &id, const std::string &file);
    bool LoadShader(const std::string& id, const std::string& file);
    // [lyrename id=… to=…] — move a layer's state to a new id.
    bool RenameLayer(const std::string& id, const std::string& to);
    bool SetPixels(const std::string& id, const uint8_t* rgba, int width, int height);
    // Capture the retained stage without redrawing the save/menu overlays.
    bool Snapshot(SnapshotImage& output) const;
    // ARCV v2 semantic scene capture; validation never mutates the compositor.
    bool CaptureScene(std::vector<SavedCommand>&, std::string&) const;
    bool ValidateScene(const std::vector<SavedCommand>&, std::string&) const;
    bool RestoreScene(const std::vector<SavedCommand>&, std::string&);
    void SetProps(const std::string &id, const std::map<std::string, std::string> &attrs);
    void DeleteLayer(const std::string &id);
    // E-mote stencil composite: render the named layers' alpha into the
    // stage-sized mask texture selected by `key` and return it (0 when GL is
    // unavailable or no layer drew). The mask is sampled in stage space by
    // masked layers; distinct keys keep distinct masks alive across a frame.
    // Callers must make the key unique per rendered source set (and owner):
    // equal keys share, and overwrite, one texture.
    uint32_t RenderStageMask(const std::string &key, const std::vector<std::string> &ids);
    uint32_t RenderStageMask(int key, const std::vector<std::string> &ids);
    // Free the mask textures whose key starts with `prefix` (no-op without GL).
    // Layers still pointing at them must be re-attached or cleared first.
    void ReleaseStageMasks(const std::string &prefix);
    // Attach (texture != 0) or clear (texture == 0) the stage mask of a layer.
    void SetLayerStageMask(const std::string &id, uint32_t texture);
    // Explicit same-Z owner-local extension (smaller = earlier/behind). Never
    // changes ordinary order or crosses an owner/ancestor effect group.
    void SetLayerPaint(const std::string &id, int paint);
    // Legacy diagnostic membership marker; ordering requires an explicit owner.
    void SetLayerEmoteTree(const std::string &id, bool on);
    // Register the E-mote scene's external ID, same-Z compatibility rank and
    // sampled effective Z. Inside an effect boundary: Z, explicit paint, rank,
    // deterministic ID. Postorder at equal Z is not fully officially confirmed.
    // All descendants of owner (including ordinary attached layers) share one
    // contiguous draw block. A negative rank uses the ID-based fallback.
    // Returns false without mutation for absent/nonmember IDs or overlapping
    // distinct owners or non-finite Z. Empty owner clears rank/paint/Z. Owners
    // exist only while a valid registered layer resides in their subtree.
    // Rename maps whole owners; moving part of one out clears its registration.
    // A whole-owner rename conflicting with another owner fails atomically.
    bool SetLayerEmoteOrder(const std::string& id, const std::string& owner, int order,
                            double effective_z=0);

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
    void SetMessageLayered(const std::string& id, bool layered);
    // [glyph homing=1] — move the glyph layer so its origin sits at the
    // end-of-text pen of the message layer, even when the two live under
    // different parent chains (e.g. text on "adv", glyph at "1.90" under
    // the window group "1"). Materializes a texture-less glyph holder when
    // the script has not created one yet. Returns false without a text layer.
    bool HomeLayerToTextPen(const std::string& text_id, const std::string& glyph_id);
    double PendingTextMs(double now_ms) const;
    bool FinishText(double now_ms);
    // Override a layer's geometry with a warped triangle list (x,y,u,v per
    // vertex, layer-local pixels). Empty restores the plain quad.
    void SetLayerMesh(const std::string& id, const std::vector<float>& vertices);
    // X=a*x+c*y+tx, Y=b*x+d*y+ty. Finite singular matrices are valid.
    // Absent IDs/invalid values return false without mutation. Anchors, content
    // offsets and live ancestors still apply. Leaves legacy TRS channels intact.
    bool SetLayerAffine(const std::string& id, float a, float b, float c, float d,
                        float tx, float ty);
    // Restore stored legacy TRS/reverse, retaining current x/y and anchor.
    // Explicit rotate/scale/zoom/reverse property or tween writes also clear it;
    // position/alpha/effect writes do not. No-op success does not bump revision.
    bool ClearLayerAffine(const std::string& id);

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
    // RAII: while alive, ancestor transforms are memoized per layer, so a pass
    // over all layers costs O(N) instead of O(N*depth). Only valid while the
    // layer table and layer properties are not mutated (Draw, hit tests,
    // per-frame read-only scans). Nestable; results are bit-identical.
    class TransformCache {
    public:
        explicit TransformCache(const Compositor& c);
        ~TransformCache();
        TransformCache(const TransformCache&) = delete;
        TransformCache& operator=(const TransformCache&) = delete;
    private:
        const Compositor& c_;
    };
    // Complexity counters for regression tests (not behavior).
    struct PerfCounters {
        uint64_t index_rebuilds = 0;    // id->layer index rebuilds
        uint64_t owner_rebuilds = 0;    // registered E-mote owner list rebuilds
        uint64_t index_lookups = 0;     // id->layer lookups served by the index
        uint64_t transform_applies = 0; // per-layer matrix compositions
    };
    const PerfCounters& Counters() const { return counters_; }
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
    // [anime] frame animation: init/add accumulate (time,file,props) frames,
    // end sorts and starts playback. Frames switch the layer image/props.
    void SetAnimeFrame(const std::string &id, const std::string &mode,
                       const std::string &file, int time_ms, int loop,
                       const std::map<std::string, std::string> &props, double now_ms);
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
    friend struct CompositorOrderRegression;
    struct DrawOrderPart {
        double effective_z=0;
        int paint=0, order=-1;
        std::string_view id;
    };
    struct DrawOrderEntry {
        std::string_view id;
        std::string_view external;
        bool overlay=false;
        std::vector<DrawOrderPart> path;
    };
    struct DrawOrderScratch {
        std::vector<DrawOrderEntry> entries;
    };
    // id -> layer index over layers_ (first match wins, later duplicates are
    // chained), nearest existing dotted-prefix ancestor per layer, and the
    // sorted distinct valid E-mote owners. Rebuilt lazily; every mutation of
    // layers_ (push/erase/clear/rename) must call InvalidateLayerIndex(), and
    // every change of an emote_owner must call InvalidateEmoteOwners().
    // Index() also self-checks data pointer and size as a safety net.
    static constexpr uint32_t kNoLayer = ~0u;
    struct LayerIndex {
        bool valid = false, owners_valid = false;
        const Layer* data = nullptr;
        size_t size = 0;
        uint32_t mask = 0;
        std::vector<uint32_t> slots;      // open addressing: 0 empty, else index+1
        std::vector<uint32_t> parent;     // nearest existing ancestor or kNoLayer
        std::vector<uint32_t> next_same;  // next layer with the same id
        std::vector<uint32_t> tail;       // last layer of a duplicate chain (by first)
        std::vector<std::string_view> owners;
    };
    struct WorldEntry {
        Transform m;
        uint32_t stamp = 0;
    };
    const LayerIndex& Index() const;
    void RebuildLayerIndex() const;
    void InvalidateLayerIndex() { layer_index_.valid = false; layer_index_.owners_valid = false; }
    void InvalidateEmoteOwners() { layer_index_.owners_valid = false; }
    static uint32_t LookupIn(const LayerIndex& ix, const std::vector<Layer>& layers,
                             std::string_view id);
    uint32_t FindLayer(std::string_view id) const;
    uint32_t NearestAncestor(std::string_view id) const;
    const std::vector<std::string_view>& EmoteOwners() const;
    Transform WorldTransform(uint32_t index) const;
    static Transform ComposeLayer(const Transform& parent, const Layer& layer);
    mutable LayerIndex layer_index_;
    mutable std::vector<WorldEntry> world_cache_;
    mutable uint32_t world_stamp_ = 0;
    mutable int world_scope_ = 0;
    mutable PerfCounters counters_;
    void BuildDrawList(std::vector<const Layer*>& sorted,
                       DrawOrderScratch& scratch) const;
    static bool DrawOrderLess(const DrawOrderEntry& a,const DrawOrderEntry& b);
    DrawOrderScratch draw_order_scratch_;
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
    bool IsProhibitHead(uint32_t cp) const;
    bool IsProhibitFoot(uint32_t cp) const;
    bool IsWordpart(uint32_t cp) const;
    // Glyph cache GL side: page textures keyed by atlas page index, with the
    // uploaded generation per page (a page re-uploads only when it changed).
    uint32_t GlyphPageTexture(size_t page);
    void DropGlyphGl();

    int stage_w_ = 1280, stage_h_ = 720;
    LayerShaders shaders_;
    std::string save_directory_;
    struct MaskTexture {uint32_t texture=0;int width=0,height=0;};
    std::map<std::string,MaskTexture> masks_;
    void LoadMask(const std::string& file);
    GlProgram prog_{};
    TransProgram tprog_{};
    bool gl_ready_ = false;
    // E-mote stencil composite: a stage-sized alpha texture rendered
    // from the scene's mask layers and sampled per masked layer. One target
    // per mask label set (keyed by the caller) so several masks can coexist.
    struct StageMaskTarget { uint32_t fbo = 0, texture = 0; };
    std::map<std::string, StageMaskTarget> stage_masks_;
    void ReleaseStageMaskGl();

    // [prohibit]/[wordparts]/[indent] configuration (empty = documented default)
    std::set<uint32_t> prohibit_head_, prohibit_foot_, wordparts_;
    std::string indent_pair_;
    int indent_range_ = -1;
    bool indent_nest_ = false;

    std::vector<Tween> tweens_;
    bool collecting_tweens_ = false;
    std::vector<Tween> tween_set_;
    // [anime] frame animation state per layer.
    struct AnimeFrame {
        double time_ms = 0;
        std::string file;
        std::map<std::string, std::string> props;
    };
    struct AnimeState {
        std::vector<AnimeFrame> frames;
        int loop = -1;
        double start_ms = 0;
        double total_ms = 0;
        std::string active_file;
        int active_index = -1;
    };
    std::map<std::string, AnimeState> anime_;
    void AdvanceAnime(double now_ms, bool *changed);
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

    // persistent glyph cache (see glyph_atlas.h): rasterized cells survive
    // SetText calls; the compositor owns page textures + upload generations.
    GlyphAtlas glyph_atlas_;
    // Draw() scratch, reused across frames (T2-2): the hot path must not
    // allocate per frame. Vector clear() keeps capacity, so a stable scene
    // (and a steadily-changing one) reuses the same storage.
    std::vector<const Layer *> draw_sorted_;
    std::vector<std::pair<std::string, uint32_t>> draw_textures_;
    uint64_t draw_textures_rev_ = ~0ull;   // revision the table was built at
    std::vector<std::pair<uint32_t, std::vector<float>>> draw_glyph_runs_;
    std::vector<float> draw_mesh_vertices_;
    std::map<size_t, uint32_t> glyph_page_textures_;
    std::map<size_t, uint64_t> glyph_page_uploaded_;
};

} // namespace artc
