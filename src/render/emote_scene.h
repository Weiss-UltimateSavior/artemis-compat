#pragma once
#include "render/emote_model.h"
#include "render/emote_mesh.h"
#include <memory>

namespace artc {
class Compositor;
struct EmoteSceneLayer {
    std::string key,source,icon;
    // Node label (shape layers are addressed by it in hit tests) and the
    // hit-test geometry of `shape/...` / type==1 nodes.
    std::string label;
    // Content blend mode (`bm` low nibble, E-mote): empty = normal/alpha,
    // otherwise "add", "subtract", "multiply" or "screen".
    std::string blend;
    // Stencil composite mask layer labels active for this part (empty = no
    // mask). Render resolves them to the evaluator's mask source layers.
    std::vector<std::string> masks;
    // Shape-sync deformers inherited from ancestor meshTransform nodes
    // (nearest first). Render bakes them into an 8x8 warped mesh.
    struct Deformer {
        double m11=1,m12=0,m21=0,m22=1,tx=0,ty=0;  // part/node space -> domain
        std::vector<double> points;                // normalized control grid
        int side=0;
        double width=0,height=0;                   // extent size
        double origin_x=0,origin_y=0;              // extent origin
        double off_x=0,off_y=0;                    // content ox/oy offset
        // Extent source when the sync node draws a real icon instead of a
        // blank descriptor: resolved from the icon atlas at render time.
        std::string source,icon;
    };
    // Absolute affine of this node inside the E-mote player. Native E-mote
    // places every node inside a motion by its own coordinate (the hierarchy
    // does not accumulate transforms); Render composes the icon offset.
    double abs_m11=1,abs_m12=0,abs_m21=0,abs_m22=1,abs_tx=0,abs_ty=0;
    // Node linear state after inheritMask resolution (used when this node
    // enters a nested motion player).
    double st_rot=0,st_sx=1,st_sy=1,st_shx=0,st_shy=0;
    bool st_fx=false,st_fy=false;
    // Node opacity after inheritMask resolution (nested motion roots).
    double abs_opa=1;
    // Provisional draw-order hint derived from the authored labels: back hair
    // and the skin patches layered under it draw behind the portrait, and the
    // neck draws behind the chin (observed official shell ordering).
    int paint_hint=0;
    std::vector<double> transform_order;
    std::vector<Deformer> deformers;
    enum class Shape { None, Rect, Circle, Point, Quad };
    Shape shape=Shape::None;
    // Per-frame color (0xRRGGBBAA, MODULATE2X space); 0 = not authored.
    uint32_t color=0;
    double x=0,y=0,angle=0,scale_x=1,scale_y=1,opacity=1,origin_x=0,origin_y=0;
    bool visible=true;
    // Per-icon Bezier mesh warp: interleaved (warped_x, warped_y, u, v) in
    // normalized icon space (triangles, 4 floats per vertex). Empty = affine.
    std::vector<float> mesh;
};
// Frame evaluator/renderer for plain image/layout/child-motion nodes plus
// per-icon Bezier mesh warp (DXT5/BC7/RGBA8 atlases). Stencil/mask composites,
// physics and non-per-icon mesh propagation are still rejected before changing
// the displayed scene; this does not advertise complete SDK support.
class EmoteScene {
public:
    bool Load(std::shared_ptr<const EmoteModel> model,std::string& error);
    // mesh_side selects the output subdivision of warped mesh patches
    // (setMeshDivisionRatio; kEmoteMeshSide is the full-quality default).
    bool Evaluate(double frame,const std::map<std::string,double>& variables,
                  std::vector<EmoteSceneLayer>& output,std::string& error,
                  int mesh_side=kEmoteMeshSide) const;
    // grayscale (0..1) is applied to the textured part layers when > 0; the
    // E-mote player's SetGrayscale drives it. multiply is a straight RGB
    // factor for 0x00RRGGBB (0xFFFFFF = neutral); the player converts the
    // native MODULATE2X tint before calling. mesh_division scales the mesh
    // subdivision (1.0 = full, lower = coarser; the scripts' performance knob).
    bool Render(Compositor& compositor,const std::string& id,double frame,
                const std::map<std::string,double>& variables,std::string& error,
                double grayscale=0,uint32_t multiply=0xFFFFFF,double mesh_division=1.0);
    // Point test over the shape layers installed under id; an empty label
    // matches every shape (any hit). Coordinates are stage pixels and the
    // compositor supplies the live transform chain.
    bool HitTest(Compositor& compositor,const std::string& id,const std::string& label,
                 double x,double y) const;
    // Delete every layer this scene installed under id (the bare container id
    // itself is the caller's — the player removes it via its own RemoveLayers).
    void Remove(Compositor& compositor,const std::string& id);
private:
    std::shared_ptr<const EmoteModel> model_;
    std::map<std::pair<std::string,std::string>,EmoteImage> images_;
    std::map<std::string,std::pair<std::string,std::string>> installed_;
    std::set<std::string> installed_layers_;
    // metadata.attrcomp removals, keyed "chara\nmotion\nlayer"; nodes matching
    // one are skipped by both validation and evaluation.
    std::set<std::string> removed_;
    struct InstalledShape {
        std::string id,label;
        EmoteSceneLayer::Shape shape=EmoteSceneLayer::Shape::None;
    };
    std::vector<InstalledShape> installed_shapes_;
    static std::string RemovalKey(const std::string& chara,const std::string& motion,
                                  const std::string& layer) {
        return chara+'\n'+motion+'\n'+layer;
    }
};
}
