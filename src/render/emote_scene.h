#pragma once
#include "render/emote_model.h"
#include <memory>

namespace artc {
class Compositor;
struct EmoteSceneLayer {
    std::string key,source,icon;
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
    bool Evaluate(double frame,const std::map<std::string,double>& variables,
                  std::vector<EmoteSceneLayer>& output,std::string& error) const;
    bool Render(Compositor& compositor,const std::string& id,double frame,
                const std::map<std::string,double>& variables,std::string& error);
    // Delete every layer this scene installed under id (the bare container id
    // itself is the caller's — the player removes it via its own RemoveLayers).
    void Remove(Compositor& compositor,const std::string& id);
private:
    std::shared_ptr<const EmoteModel> model_;
    std::map<std::pair<std::string,std::string>,EmoteImage> images_;
    std::map<std::string,std::pair<std::string,std::string>> installed_;
    std::set<std::string> installed_layers_;
};
}
