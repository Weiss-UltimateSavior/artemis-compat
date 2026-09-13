#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace artc {
struct LayerEffect {
    std::string shader, blend="normal", mask;
    bool negative=false, grayscale=false;
    uint32_t multiply=0xffffff;
    int intermediate=0;
    std::map<std::string,std::string> parameters;
    bool Active() const {
        return !shader.empty() || blend!="normal" || negative || grayscale ||
               multiply!=0xffffff || intermediate!=0 || !mask.empty();
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
             float opacity,const std::map<std::string,uint32_t>& textures,const LayerCoverage& coverage={});
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
