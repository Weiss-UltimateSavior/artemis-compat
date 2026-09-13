#include "render/layer_shader.h"
#include "log/logger.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <sstream>
#if defined(ARTC_HAS_GLES)
#include <GLES2/gl2.h>
#endif
namespace artc {
void LayerEffect::Set(const std::map<std::string,std::string>& attrs) {
    for(const auto& v:attrs) {
        parameters[v.first]=v.second;
        if(v.first=="shader")shader=v.second;
        else if(v.first=="negative")negative=v.second!="0";
        else if(v.first=="grayscale")grayscale=v.second!="0";
        else if(v.first=="colormultiply")multiply=std::strtoul(v.second.c_str(),nullptr,0)&0xffffff;
        else if(v.first=="layermode")blend=v.second.empty()?"normal":v.second;
        else if(v.first=="intermediate_render")intermediate=std::atoi(v.second.c_str());
        else if(v.first=="intermediate_render_mask")mask=v.second;
    }
}
#if defined(ARTC_HAS_GLES)
namespace {
const char* vertex=R"(
attribute vec2 a_pos;
attribute vec2 a_uv;
uniform float artc_top_down;
varying vec2 resultCoord0;
varying vec2 resultCoord1;
void main() {
    gl_Position=vec4(a_pos.x,mix(-a_pos.y,a_pos.y,artc_top_down),0.0,1.0);
    resultCoord0=a_uv;resultCoord1=a_uv;
})";
const char* copy=R"(
precision highp float;
varying vec2 resultCoord1;
uniform sampler2D textureFore;
uniform float flip;
void main() {
    vec2 uv=resultCoord1;uv.y=mix(uv.y,1.0-uv.y,flip);
    vec4 c=texture2D(textureFore,uv);
    gl_FragColor=vec4(c.a>0.0?c.rgb/c.a:vec3(0.0),c.a);
})";
const char* builtin=R"(
precision highp float;
varying vec2 resultCoord1;
uniform sampler2D textureFore;
uniform vec3 colorMultiply;
uniform float negative;
uniform float grayscale;
void main() {
    vec4 c=texture2D(textureFore,resultCoord1);
    c.rgb=mix(c.rgb,vec3(1.0)-c.rgb,negative);
    c.rgb=mix(c.rgb,vec3(dot(c.rgb,vec3(0.298912,0.586611,0.114478))),grayscale);
    gl_FragColor=vec4(c.rgb*colorMultiply,c.a);
})";
const char* coverage_fragment=R"(
precision highp float;
varying vec2 resultCoord1;
uniform sampler2D artc_fore;
uniform sampler2D artc_mask;
uniform vec2 artc_stage;
uniform mat3 artc_inverse;
uniform vec4 artc_rect;
uniform vec2 artc_mask_size;
uniform float artc_clip;
uniform float artc_use_mask;
void main() {
    vec2 local=(artc_inverse*vec3(resultCoord1*artc_stage,1.0)).xy;
    float alpha=1.0;
    if(artc_clip>0.5 && (local.x<artc_rect.x || local.y<artc_rect.y ||
       local.x>=artc_rect.x+artc_rect.z || local.y>=artc_rect.y+artc_rect.w))alpha=0.0;
    if(artc_use_mask>0.5) {
        vec2 uv=local/artc_mask_size;
        if(uv.x<0.0 || uv.y<0.0 || uv.x>=1.0 || uv.y>=1.0)alpha=0.0;
        else {vec4 m=texture2D(artc_mask,uv);alpha*=m.r*m.a;}
    }
    gl_FragColor=texture2D(artc_fore,resultCoord1)*alpha;
})";
std::vector<float> Numbers(std::string s) {
    std::replace(s.begin(),s.end(),',',' ');std::istringstream in(s);
    std::vector<float> v;float f;
    while(in>>f){if(!std::isfinite(f)||v.size()>=256)return {};v.push_back(f);}
    return in.eof()?v:std::vector<float>{};
}
void Uniform(uint32_t p,const char* name,float value) {glUniform1f(glGetUniformLocation(p,name),value);}
}
uint32_t LayerShaders::Compile(const std::string& source,bool wrap) {
    std::string fragment=source;
    if(wrap) {
        // Keep an optional #version as the first directive. Game shaders produce
        // straight RGBA; the wrapper applies group opacity exactly once and
        // premultiplies for normal/add/screen composition.
        size_t prefix=0;
        if(fragment.rfind("#version",0)==0){prefix=fragment.find('\n');if(prefix==std::string::npos)return 0;++prefix;}
        fragment.insert(prefix,"#define main artc_game_main\n");
        fragment+="\n#undef main\nuniform lowp float artc_opacity;\nvoid main(){artc_game_main();gl_FragColor.a*=artc_opacity;gl_FragColor.rgb*=gl_FragColor.a;}\n";
    }
    auto shader=[](GLenum type,const std::string& code)->GLuint {
        GLuint id=glCreateShader(type);const char* s=code.c_str();glShaderSource(id,1,&s,nullptr);glCompileShader(id);
        GLint ok=0;glGetShaderiv(id,GL_COMPILE_STATUS,&ok);
        if(!ok){char log[2048]={};glGetShaderInfoLog(id,sizeof(log),nullptr,log);
            Log(kLogError,std::string("layer shader compile: ")+log);glDeleteShader(id);return 0;}
        return id;
    };
    const GLuint vs=shader(GL_VERTEX_SHADER,vertex),fs=shader(GL_FRAGMENT_SHADER,fragment);
    if(!vs||!fs){if(vs)glDeleteShader(vs);if(fs)glDeleteShader(fs);return 0;}
    GLuint p=glCreateProgram();glAttachShader(p,vs);glAttachShader(p,fs);
    glBindAttribLocation(p,0,"a_pos");glBindAttribLocation(p,1,"a_uv");glLinkProgram(p);
    glDeleteShader(vs);glDeleteShader(fs);GLint ok=0;glGetProgramiv(p,GL_LINK_STATUS,&ok);
    if(!ok){char log[2048]={};glGetProgramInfoLog(p,sizeof(log),nullptr,log);
        Log(kLogError,std::string("layer shader link: ")+log);glDeleteProgram(p);return 0;}
    return p;
}
bool LayerShaders::Load(const std::string& id,const std::string& source) {
    if(id.empty() || source.empty() || source.size()>1024*1024)return false;
    const auto p=Compile(source,true);if(!p)return false;
    auto& old=programs_[id];if(old.gl)glDeleteProgram(old.gl);old={p,source};return true;
}
void LayerShaders::ReleaseGl() {
    for(auto& p:programs_) {if(p.second.gl)glDeleteProgram(p.second.gl);p.second.gl=0;}
    for(auto& g:groups_)for(auto* s:{&g.raw,&g.fore,&g.back,&g.capture}) {
        if(s->fbo)glDeleteFramebuffers(1,&s->fbo);if(s->texture)glDeleteTextures(1,&s->texture);
    }
    groups_.clear();
    if(copy_)glDeleteProgram(copy_);if(builtin_)glDeleteProgram(builtin_);if(white_)glDeleteTextures(1,&white_);
    if(coverage_)glDeleteProgram(coverage_);
    copy_=builtin_=white_=coverage_=0;
}
bool LayerShaders::Allocate(Group& g,int w,int h) {
    if(g.width==w && g.height==h && g.raw.fbo)return true;
    if(w<=0||h<=0||uint64_t(w)*h>16*1024*1024)return false;
    for(auto* s:{&g.raw,&g.fore,&g.back,&g.capture}) {
        if(!s->texture)glGenTextures(1,&s->texture);if(!s->fbo)glGenFramebuffers(1,&s->fbo);
        glBindTexture(GL_TEXTURE_2D,s->texture);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glBindFramebuffer(GL_FRAMEBUFFER,s->fbo);glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->texture,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE)return false;
    }
    g.width=w;g.height=h;return true;
}
void LayerShaders::Quad(uint32_t p,const Surface& target,int w,int h,bool top_down) {
    glBindFramebuffer(GL_FRAMEBUFFER,target.fbo);glViewport(0,0,w,h);glUseProgram(p);
    Uniform(p,"artc_top_down",top_down?1:0);
    // a_pos.y is in top-down clip coordinates, a_uv in native texture coordinates.
    const float v[]={-1,-1,0,0, 1,-1,1,0, -1,1,0,1, 1,1,1,1};
    glDisableVertexAttribArray(2);glVertexAttrib1f(2,1);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,16,v);glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,2,GL_FLOAT,GL_FALSE,16,v+2);glEnableVertexAttribArray(1);
    glDrawArrays(GL_TRIANGLE_STRIP,0,4);
}
uint32_t LayerShaders::Begin(size_t depth,int w,int h,uint32_t parent,bool top_down) {
    if(depth>=32)return 0;
    if(groups_.size()<=depth)groups_.resize(depth+1);
    auto& g=groups_[depth];
    if(!copy_)copy_=Compile(copy,false);if(!builtin_)builtin_=Compile(builtin,true);
    if(!copy_||!builtin_||!Allocate(g,w,h)){glBindFramebuffer(GL_FRAMEBUFFER,parent);return 0;}
    glActiveTexture(GL_TEXTURE0);glBindFramebuffer(GL_FRAMEBUFFER,parent);
    glBindTexture(GL_TEXTURE_2D,g.capture.texture);glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,w,h);
    glDisable(GL_BLEND);glUseProgram(copy_);glUniform1i(glGetUniformLocation(copy_,"textureFore"),0);
    Uniform(copy_,"flip",top_down?0:1);Quad(copy_,g.back,w,h,true);
    glBindFramebuffer(GL_FRAMEBUFFER,g.raw.fbo);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
    return g.raw.fbo;
}
bool LayerShaders::End(size_t depth,const LayerEffect& effect,uint32_t parent,bool top_down,
                       float opacity,const std::map<std::string,uint32_t>& textures,const LayerCoverage& coverage) {
    auto& g=groups_.at(depth);
    glDisable(GL_BLEND);glUseProgram(copy_);Uniform(copy_,"flip",0);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g.raw.texture);
    Quad(copy_,g.fore,g.width,g.height,true);
    uint32_t p=builtin_;
    if(!effect.shader.empty()) {
        auto it=programs_.find(effect.shader);
        if(it!=programs_.end()) {
            if(!it->second.gl)it->second.gl=Compile(it->second.source,true);
            if(it->second.gl)p=it->second.gl;
        }
    }
    glUseProgram(p);
    if(!white_) {
        const uint8_t rgba[4]={255,255,255,255};glGenTextures(1,&white_);glBindTexture(GL_TEXTURE_2D,white_);
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,rgba);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    GLint count=0,max_units=0;glGetProgramiv(p,GL_ACTIVE_UNIFORMS,&count);glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS,&max_units);
    int unit=0;
    for(int i=0;i<count;++i) {
        char name[256]={};GLint length=0;GLenum type=0;glGetActiveUniform(p,i,sizeof(name),nullptr,&length,&type,name);
        std::string key=name;auto bracket=key.find('[');if(bracket!=std::string::npos)key.resize(bracket);
        GLint location=glGetUniformLocation(p,name);
        if(type==GL_SAMPLER_2D) {
            if(length>max_units-unit) {
                glBindFramebuffer(GL_FRAMEBUFFER,parent);glViewport(0,0,g.width,g.height);
                Log(kLogError,"layer shader exceeds texture unit limit");return false;
            }
            std::vector<GLint> units;
            for(int n=0;n<length;++n) {
                uint32_t texture=white_;
                if(key=="textureFore")texture=g.fore.texture;
                else if(key=="textureBack")texture=g.back.texture;
                else {
                    auto binding=effect.parameters.find(length>1?key+"["+std::to_string(n)+"]":key);
                    if(binding==effect.parameters.end() && n==0)binding=effect.parameters.find(key);
                    if(binding!=effect.parameters.end())if(auto found=textures.find(binding->second);found!=textures.end())texture=found->second;
                }
                glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,texture);units.push_back(unit++);
            }
            glUniform1iv(location,length,units.data());continue;
        }
        // Native standard uniforms are set below, independently of arbitrary parameters.
        if(key=="artc_top_down" || key=="artc_opacity" || key=="alpha" || key=="colorMultiply")continue;
        std::vector<float> v;
        if(auto it=effect.parameters.find(key);it!=effect.parameters.end())v=Numbers(it->second);
        int width=1;
        if(type==GL_FLOAT_VEC2 || type==GL_INT_VEC2 || type==GL_BOOL_VEC2)width=2;
        else if(type==GL_FLOAT_VEC3 || type==GL_INT_VEC3 || type==GL_BOOL_VEC3)width=3;
        else if(type==GL_FLOAT_VEC4 || type==GL_INT_VEC4 || type==GL_BOOL_VEC4)width=4;
        else if(type==GL_FLOAT_MAT2)width=4;else if(type==GL_FLOAT_MAT3)width=9;else if(type==GL_FLOAT_MAT4)width=16;
        v.resize(size_t(length)*width,0); // no uniform state leaks from another layer
        switch(type) {
        case GL_FLOAT:glUniform1fv(location,length,v.data());break;
        case GL_FLOAT_VEC2:glUniform2fv(location,length,v.data());break;
        case GL_FLOAT_VEC3:glUniform3fv(location,length,v.data());break;
        case GL_FLOAT_VEC4:glUniform4fv(location,length,v.data());break;
        case GL_FLOAT_MAT2:glUniformMatrix2fv(location,length,GL_FALSE,v.data());break;
        case GL_FLOAT_MAT3:glUniformMatrix3fv(location,length,GL_FALSE,v.data());break;
        case GL_FLOAT_MAT4:glUniformMatrix4fv(location,length,GL_FALSE,v.data());break;
        case GL_INT:case GL_BOOL:case GL_INT_VEC2:case GL_BOOL_VEC2:
        case GL_INT_VEC3:case GL_BOOL_VEC3:case GL_INT_VEC4:case GL_BOOL_VEC4: {
            const bool boolean=type==GL_BOOL || type==GL_BOOL_VEC2 || type==GL_BOOL_VEC3 || type==GL_BOOL_VEC4;
            std::vector<GLint> n;n.reserve(v.size());
            for(float x:v)n.push_back(boolean?x!=0:GLint(std::clamp(double(x),double(std::numeric_limits<GLint>::min()),double(std::numeric_limits<GLint>::max()))));
            if(width==1)glUniform1iv(location,length,n.data());else if(width==2)glUniform2iv(location,length,n.data());
            else if(width==3)glUniform3iv(location,length,n.data());else glUniform4iv(location,length,n.data());break;
        }
        default: break;
        }
    }
    Uniform(p,"alpha",1);Uniform(p,"artc_opacity",std::clamp(opacity,0.f,1.f));
    glUniform3f(glGetUniformLocation(p,"colorMultiply"),((effect.multiply>>16)&255)/255.f,
                ((effect.multiply>>8)&255)/255.f,(effect.multiply&255)/255.f);
    if(p==builtin_){Uniform(p,"negative",effect.negative);Uniform(p,"grayscale",effect.grayscale);}
    if(coverage.Active()) {
        if(!coverage_)coverage_=Compile(coverage_fragment,false);
        if(!coverage_){glBindFramebuffer(GL_FRAMEBUFFER,parent);return false;}
        // The raw input has already been resolved to fore, so its surface can
        // hold the processed premultiplied result without a texture feedback loop.
        glDisable(GL_BLEND);Quad(p,g.raw,g.width,g.height,true);
        p=coverage_;glUseProgram(p);
        glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,g.raw.texture);
        glUniform1i(glGetUniformLocation(p,"artc_fore"),0);
        glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,coverage.mask?coverage.mask:white_);
        glUniform1i(glGetUniformLocation(p,"artc_mask"),1);
        glUniform2f(glGetUniformLocation(p,"artc_stage"),g.width,g.height);
        glUniformMatrix3fv(glGetUniformLocation(p,"artc_inverse"),1,GL_FALSE,coverage.inverse);
        glUniform4fv(glGetUniformLocation(p,"artc_rect"),1,coverage.rect);
        glUniform2f(glGetUniformLocation(p,"artc_mask_size"),std::max(1,coverage.mask_width),std::max(1,coverage.mask_height));
        Uniform(p,"artc_clip",coverage.clip);Uniform(p,"artc_use_mask",coverage.mask!=0);
    }
    glEnable(GL_BLEND);
    if(effect.blend=="add")glBlendFuncSeparate(GL_ONE,GL_ONE,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    else if(effect.blend=="screen")glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_COLOR,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    else glBlendFuncSeparate(GL_ONE,GL_ONE_MINUS_SRC_ALPHA,GL_ONE,GL_ONE_MINUS_SRC_ALPHA);
    Quad(p,Surface{0,parent},g.width,g.height,top_down);
    return true;
}
#else
bool LayerShaders::Load(const std::string& id,const std::string& source) {
    if(id.empty()||source.empty())return false;programs_[id]={0,source};return true;
}
void LayerShaders::ReleaseGl() {}
uint32_t LayerShaders::Begin(size_t,int,int,uint32_t,bool){return 0;}
bool LayerShaders::End(size_t,const LayerEffect&,uint32_t,bool,float,const std::map<std::string,uint32_t>&,const LayerCoverage&){return false;}
#endif
}
