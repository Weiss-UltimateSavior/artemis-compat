#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
namespace artc {
struct LayerEffect {
    std::string shader, blend="normal", mask;
    bool negative=false;
    // 0 = off, 1 = full luminance; the builtin shader mixes by this weight so
    // E-mote's SetGrayscale can animate the desaturation.
    float grayscale=0.0f;
    uint32_t multiply=0xffffff;
    int intermediate=0;
    // Explicit draw-order hint (smaller = earlier/behind). 0 = default order.
    // E-mote rear layers (back hair) use -1; the compositor sorts by this
    // before the id order while everything else keeps the id semantics.
    int paint=0;
    // E-mote stencil composite: the alpha of this stage-space mask texture is
    // multiplied into the layer (0 = no mask). Set through
    // Compositor::SetLayerStageMask / RenderStageMask, not through props.
    uint32_t stage_mask=0;
    std::map<std::string,std::string> parameters;
    bool Active() const {
        return !shader.empty() || blend!="normal" || negative || grayscale>0.0f ||
               multiply!=0xffffff || intermediate!=0 || !mask.empty() || stage_mask!=0;
    }
    void Set(const std::map<std::string,std::string>& attrs);
};
struct LayerCoverage {
    // Column-major stage-to-local affine transform. Clip/mask coordinates are
    // local to the effect group, so they follow its parent transforms.
    float inverse[9]={1,0,0,0,1,0,0,0,1};
    bool clip=false;
    float rect[4]={0,0,0,0};
    uint32_t mask=0;
    int mask_width=0,mask_height=0;
    // Stage masks (E-mote stencil composites) store coverage in the alpha
    // channel; file masks carry it in red*alpha.
    bool mask_alpha=false;
    bool Active() const {return clip || mask;}
};
// GLES2 implementation of the native mobile shader ABI. Intermediate surfaces
// use top-down texture coordinates; their RGB is premultiplied until resolved
// to the straight-alpha textureFore expected by game-authored GLSL programs.
class LayerShaders {
public:
    bool Load(const std::string& id,const std::string& source);
    void ReleaseGl();
    uint32_t Begin(size_t depth,int width,int height,uint32_t parent,bool parent_top_down);
    bool End(size_t depth,const LayerEffect& effect,uint32_t parent,bool parent_top_down,
             float opacity,const std::vector<std::pair<std::string,uint32_t>>& textures,const LayerCoverage& coverage={});
private:
    struct Program { uint32_t gl=0;std::string source; };
    struct Surface { uint32_t texture=0,fbo=0; };
    struct Group { Surface raw,fore,back,capture;int width=0,height=0; };
    std::map<std::string,Program> programs_;
    std::vector<Group> groups_;
    uint32_t copy_=0,builtin_=0,white_=0,coverage_=0;
    uint32_t Compile(const std::string& source,bool wrap);
    bool Allocate(Group& group,int width,int height);
    void Quad(uint32_t program,const Surface& target,int width,int height,bool top_down);
};
}
