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
#include <functional>
#include <map>
#include <string>
#include <vector>
#include <set>

namespace artc {

class PackManager;

struct Layer {
    std::string id;
    uint32_t texture = 0;      // GL texture name (0 = no texture)
    int tex_w = 0, tex_h = 0;
    float x = 0, y = 0;        // draw position (anchor applied)
    float w = 0, h = 0;        // display size in stage units (0 = natural size)
    float alpha = 1.0f;
    bool visible = true;
    int z = 0;                 // higher = closer to viewer (ID leading number)
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;  // clip region (normalized UV)
    float ax = 0, ay = 0;      // anchor point within the image
    bool own_pos = false;      // lyprop set an explicit position on this layer
    // [lyprop draggable/dragarea] — the framework's slider pins are
    // draggable within a rect given as {left, top, right, bottom} offsets
    // in stage units (relative to the layer's own origin).
    bool draggable = false;
    float drag_l = 0, drag_t = 0, drag_r = 0, drag_b = 0;
    bool has_dragarea = false;
};

class Compositor {
public:
    void Init(int stage_w, int stage_h);   // stage resolution from system.ini
    void Shutdown();

    // Tag handlers (called from the Lua bridge on the engine thread).
    void SetPackManager(PackManager *packs) { packs_ = packs; }
    bool LoadImage(const std::string &id, const std::string &file);
    void SetProps(const std::string &id, const std::map<std::string, std::string> &attrs);
    void DeleteLayer(const std::string &id);

    // ---- text (M2.2 message layer pipeline) ----
    // Load a TTF/OTF from the pack chain (font/xxx.ttf in game data).
    bool LoadFont(const std::string &file);
    bool FontReady() const { return font_ready_; }
    // Rasterize a UTF-8 string into a single RGBA texture and put it on a
    // layer (message-layer style). color = 0xRRGGBB. Returns false when no
    // font is loaded / the string is empty.
    bool SetText(const std::string &id, const std::string &text, float size,
                 uint32_t color, float wrapWidth = 0);

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
    std::string HitLayer(float x, float y) const;

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

private:
    struct GlProgram {
        uint32_t program = 0;
        int a_pos = -1, a_uv = -1;
        int u_screen = -1, u_tex = -1, u_alpha = -1;
    };

    bool InitGl();
    uint32_t CreateTexture(const uint8_t *pixels, int w, int h);

    int stage_w_ = 1280, stage_h_ = 720;
    GlProgram prog_{};
    bool gl_ready_ = false;
    std::vector<Layer> layers_;   // draw order = vector order
    PackManager *packs_ = nullptr;
    std::function<void()> present_cb_;
    std::set<std::string> dumped_;   // one-shot rect dump

    // font state (stbtt_fontinfo is heap-pimpl'd to keep this header lean)
    std::vector<uint8_t> font_data_;
    void *font_info_ = nullptr;
    bool font_ready_ = false;
};

} // namespace artc
