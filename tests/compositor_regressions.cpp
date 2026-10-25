#include "render/compositor.h"
#include "render/layer_shader.h"
#include "render/emote_scene.h"
#include "render/stb_image.h"
#include "emote_scene_fixture.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "render/video_player.h"
#include "audio/audio.h"
#if defined(ARTC_TEST_CGL)
#include <OpenGL/OpenGL.h>
#include "render/gles2_headers.h"
#else
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#endif
#include <array>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <algorithm>
#include <new>

// Scope allocation counting to the warmed CPU ordering path, not GL drivers,
// text transforms, diagnostic formatting, or fixture construction.
static thread_local bool g_count_order_allocations=false;
static thread_local size_t g_order_allocations=0;
void* operator new(std::size_t size) {
    if(g_count_order_allocations)++g_order_allocations;
    if(void* p=std::malloc(size?size:1))return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {return ::operator new(size);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}

static void Check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << " GL=" << glGetError() << '\n'; std::exit(1); }
}
namespace artc {
struct CompositorOrderRegression {
    static void AllocationFree(const Compositor& c) {
        std::vector<const Layer*> sorted;
        Compositor::DrawOrderScratch scratch;
        c.BuildDrawList(sorted,scratch);
        for(const auto& a:scratch.entries) {
            Check(!Compositor::DrawOrderLess(a,a),"draw comparator is irreflexive");
            for(const auto& b:scratch.entries) {
                if(a.id!=b.id)Check(Compositor::DrawOrderLess(a,b)!=Compositor::DrawOrderLess(b,a),
                    "distinct draw keys are totally ordered");
                for(const auto& d:scratch.entries)
                    if(Compositor::DrawOrderLess(a,b) && Compositor::DrawOrderLess(b,d))
                        Check(Compositor::DrawOrderLess(a,d),"draw comparator is transitive for mixed owner/effect members");
            }
        }
        g_order_allocations=0;g_count_order_allocations=true;
        for(int i=0;i<20;++i)c.BuildDrawList(sorted,scratch);
        g_count_order_allocations=false;
        Check(g_order_allocations==0,"warmed ordering, including every comparison, allocates no owning strings or scratch storage");
    }
};
}
#if defined(ARTC_TEST_CGL)
// A drawable-less CGL context has no valid default framebuffer, so "screen"
// draws go to an offscreen FBO instead (the EGL pbuffer equivalent).
static GLuint g_default_fbo = 0;
#define ARTC_DEFAULT_FBO g_default_fbo
#else
#define ARTC_DEFAULT_FBO 0
#endif
static std::array<unsigned char, 4> Pixel() {
    std::array<unsigned char, 4> p{};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
    return p;
}
static std::array<unsigned char,4> At(int x, int y) {
    std::array<unsigned char,4> p{};
    glReadPixels(x,31-y,1,1,GL_RGBA,GL_UNSIGNED_BYTE,p.data());
    return p;
}
int main() {
#if defined(ARTC_TEST_CGL)
    // macOS-native offscreen context (no ANGLE, no window).
    CGLPixelFormatObj pix = nullptr; GLint npix = 0;
    CGLPixelFormatAttribute attrs[] = {
        kCGLPFAAccelerated, kCGLPFAColorSize, (CGLPixelFormatAttribute)24,
        kCGLPFAAlphaSize, (CGLPixelFormatAttribute)8, (CGLPixelFormatAttribute)0};
    Check(!CGLChoosePixelFormat(attrs, &pix, &npix) && pix, "choose CGL pixel format");
    CGLContextObj ctx = nullptr;
    Check(!CGLCreateContext(pix, nullptr, &ctx) && ctx, "create CGL context");
    Check(CGLSetCurrentContext(ctx) == kCGLNoError, "make CGL context current");
    {
        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 256, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glGenFramebuffers(1, &g_default_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, g_default_fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        Check(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
              "screen FBO complete");
    }
#else
    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Check(eglInitialize(d, nullptr, nullptr), "initialize EGL");
    EGLint attrs[] = {EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                      EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig cfg; EGLint count;
    Check(eglChooseConfig(d, attrs, &cfg, 1, &count) && count, "choose config");
    EGLint pb[] = {EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(d,cfg,pb);
    EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext ctx = eglCreateContext(d,cfg,EGL_NO_CONTEXT,ca);
    Check(eglMakeCurrent(d,surface,surface,ctx), "make current");
#endif
    {
        artc::LayerShaders shaders;
        const std::string shader=R"(
            precision mediump float;
            varying vec2 resultCoord1;
            uniform sampler2D textureFore;
            uniform float red;
            uniform float weights[2];
            void main(){ vec4 c=texture2D(textureFore,resultCoord1);
                gl_FragColor=vec4(c.r*red+weights[0],weights[1],c.b,c.a); }
        )";
        Check(shaders.Load("custom",shader),"compile mobile GLSL interface");
        Check(!shaders.Load("custom","invalid shader"),"failed replacement keeps the previous shader");
        artc::LayerEffect effect;effect.Set({{"shader","custom"},{"red","0.5"},{"weights","0.25,0.125"}});
        glBindFramebuffer(GL_FRAMEBUFFER,ARTC_DEFAULT_FBO);glViewport(0,0,32,32);
        glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
        Check(shaders.Begin(0,32,32,ARTC_DEFAULT_FBO,false)!=0,"allocate intermediate layer");
        glClearColor(0.5,0,0,0.5);glClear(GL_COLOR_BUFFER_BIT); // premultiplied red
        Check(shaders.End(0,effect,ARTC_DEFAULT_FBO,false,1,{}),"apply native GLSL uniforms");
        auto pixel=Pixel();
        Check(abs(pixel[0]-96)<=2 && abs(pixel[1]-16)<=2 && abs(pixel[2]-128)<=2,
              "unpremultiply before custom shader, then composite once");
        // Parameter state belongs to the layer, even when two layers share a program.
        effect.parameters.clear();
        glBindFramebuffer(GL_FRAMEBUFFER,ARTC_DEFAULT_FBO);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.Begin(0,32,32,ARTC_DEFAULT_FBO,false);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.End(0,effect,ARTC_DEFAULT_FBO,false,1,{});pixel=Pixel();
        Check(pixel[0]==0 && pixel[1]==0,"shader parameters never leak between layers");
        // CPU shader sources survive context/resource recreation, as on native load.
        shaders.ReleaseGl();
        glBindFramebuffer(GL_FRAMEBUFFER,ARTC_DEFAULT_FBO);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.Begin(0,32,32,ARTC_DEFAULT_FBO,false);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        effect.Set({{"red","1"}});shaders.End(0,effect,ARTC_DEFAULT_FBO,false,1,{});
        Check(Pixel()[0]==255,"recompile retained shader after GL release");
        Check(shaders.Load("integers",R"(
            precision mediump float; uniform ivec3 tint; uniform bvec3 mask; uniform mat3 basis;
            uniform sampler2D images[2]; varying vec2 resultCoord1;
            void main(){vec3 c=vec3(tint)/255.0*vec3(mask);
                gl_FragColor=vec4(basis*c*texture2D(images[0],resultCoord1).rgb*texture2D(images[1],resultCoord1).rgb,1.0);}
        )"),"compile integer/boolean vectors and sampler array");
        effect.Set({{"shader","integers"},{"tint","128,64,255"},{"mask","-2,0,1"},{"basis","1,0,0,0,1,0,0,0,1"}});
        shaders.Begin(0,32,32,ARTC_DEFAULT_FBO,false);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        Check(shaders.End(0,effect,ARTC_DEFAULT_FBO,false,1,{}),"bind all reflected GLSL uniform types");
        pixel=Pixel();Check(abs(pixel[0]-128)<=1 && pixel[1]==0 && pixel[2]==255 && glGetError()==GL_NO_ERROR,
            "integer vectors and bool normalization reach shader without stale texture-array units");
        GLuint inputs[2];glGenTextures(2,inputs);
        const uint8_t colors[2][4]={{255,128,255,255},{128,255,255,255}};
        for(int n=0;n<2;++n) {
            glBindTexture(GL_TEXTURE_2D,inputs[n]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,colors[n]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
        effect.Set({{"images[0]","first"},{"images[1]","second"}});
        shaders.Begin(0,32,32,ARTC_DEFAULT_FBO,false);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        shaders.End(0,effect,ARTC_DEFAULT_FBO,false,1,{{"first",inputs[0]},{"second",inputs[1]}});
        pixel=Pixel();Check(abs(pixel[0]-64)<=1 && pixel[1]==0 && pixel[2]==255,"sampler array elements use distinct texture units");
        glDeleteTextures(2,inputs);
        shaders.ReleaseGl();
    }
    artc::Compositor c; c.Init(32,32);
    // Synthetic pixels only: no game assets in this regression suite.
    GLuint tex; glGenTextures(1,&tex); glBindTexture(GL_TEXTURE_2D,tex);
    const unsigned char red[4] = {255,0,0,255};
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,red);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    c.SetProps("1", {{"w","32"},{"h","32"}});
    auto& layer = const_cast<artc::Layer&>(c.Layers().front());
    layer.texture=tex; layer.tex_w=layer.tex_h=1;
    glViewport(0,0,32,32); c.Draw();
    Check(Pixel()[0] == 255, "initial red scene");
    // Simulate a native window handing out a discarded/different back buffer.
    glClearColor(0,1,0,1); glClear(GL_COLOR_BUFFER_BIT);
    Check(c.BeginTransition(0,1000,{},0,0,0), "capture retained scene");
    const unsigned char blue[4] = {0,0,255,255};
    glBindTexture(GL_TEXTURE_2D,tex);
    glTexSubImage2D(GL_TEXTURE_2D,0,0,0,1,1,GL_RGBA,GL_UNSIGNED_BYTE,blue);
    c.Update(500); c.Draw();
    auto p=Pixel();
    Check(p[0]>=125 && p[0]<=130 && p[1]==0 && p[2]>=125 && p[2]<=130,
          "transition must blend retained red and new blue, never discarded green");
    c.Update(1000); c.Draw();
    p=Pixel(); Check(p[0]==0 && p[2]==255, "completed transition shows new scene");
    c.SetProps("1",{{"alpha","1"}});
    c.Draw(); p=Pixel();
    Check(p[2]<=2 && p[3]==255, "alpha=1 means 1/255 opacity, with an opaque composed frame");
    c.SetProps("1",{{"alpha","255"}});
    c.AddTween("1",{{"param","alpha"},{"from","255"},{"to","0"},{"time","1000"}},1000);
    c.Update(1998); c.Draw();p=Pixel();
    Check(p[2]<=2,"last tween fraction must not flash to full opacity");
    c.SetProps("1",{{"xscale","0"}});
    float x,y,w,h,a;bool visible;
    c.EffectiveRect(c.Layers().front(),&x,&y,&w,&h,&a,&visible);
    Check(w==0,"zero scale must collapse a layer");
    c.SetProps("1",{{"xscale","100"},{"alpha","255"}});
    std::ifstream font(std::string(ARTC_TEST_DATA)+"/rectangle.ttf",std::ios::binary);
    Check(bool(font),"open synthetic font");
    std::vector<unsigned char> font_data{std::istreambuf_iterator<char>(font),{}};
    // Synthetic uncompressed TGA surfaces with different bounding boxes.
    auto solid = [](int w, int h) {
        std::vector<unsigned char> tga(18, 0);
        tga[2]=2; tga[12]=w; tga[14]=h; tga[16]=32; tga[17]=0x28;
        for (int i=0;i<w*h;++i) tga.insert(tga.end(), {0,0,255,255});
        return tga;
    };
    const std::string name="font.ttf";
    std::vector<std::pair<std::string,std::vector<unsigned char>>> files = {
        {name,font_data}, {"small.tga",solid(4,2)}, {"large.tga",solid(8,6)}};
    auto mask=solid(8,8);
    for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
        const size_t at=18+(y*8+x)*4;
        mask[at]=mask[at+1]=mask[at+2]=x<4?255:0;
        mask[at+3]=y<4?255:128;
    }
    files.push_back({"mask.tga",mask});
#if defined(ARTC_TEST_MOVIE)
    std::ifstream movie_file(ARTC_TEST_MOVIE,std::ios::binary);
    auto movie=std::make_shared<std::vector<uint8_t>>(std::istreambuf_iterator<char>(movie_file),std::istreambuf_iterator<char>());
    files.push_back({"movie.mp4",*movie});
#endif
    const std::string native_shader=R"(
        precision highp float;
        varying vec2 resultCoord0; varying vec2 resultCoord1;
        uniform sampler2D textureFore; uniform sampler2D textureUser;
        uniform float red; uniform float green; uniform float blue; uniform float userMix;
        void main(){vec4 c=texture2D(textureFore,resultCoord1);
            c.rgb*=vec3(red,green,blue);
            gl_FragColor=mix(c,texture2D(textureUser,resultCoord1),userMix);}
    )";
    const std::string uv_shader=R"(
        precision highp float; varying vec2 resultCoord0; varying vec2 resultCoord1;
        void main(){gl_FragColor=vec4(resultCoord0.x,resultCoord1.y,0.0,1.0);}
    )";
    files.push_back({"filter.glsl",{native_shader.begin(),native_shader.end()}});
    files.push_back({"uv.glsl",{uv_shader.begin(),uv_shader.end()}});
    // A pf8 fixture keeps the production pack/font/image loading path.
    std::vector<unsigned char> pack{'p','f','8'};
    auto u32=[&](uint32_t n){for(int i=0;i<4;++i)pack.push_back((n>>(8*i))&255);};
    uint32_t index_size=4;
    for(const auto& f:files) index_size+=16+f.first.size();
    u32(index_size);u32(files.size());
    uint32_t offset=7+index_size;
    for(const auto& f:files) {
        u32(f.first.size());pack.insert(pack.end(),f.first.begin(),f.first.end());
        u32(0);u32(offset);u32(f.second.size());offset+=f.second.size();
    }
    for(const auto& f:files) pack.insert(pack.end(),f.second.begin(),f.second.end());
    auto path=std::filesystem::temp_directory_path()/
        ("artemis-font-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pfs");
    {std::ofstream out(path,std::ios::binary);out.write((char*)pack.data(),pack.size());}
    artc::PackManager packs;Check(packs.OpenChain(path.string(),{0}),"open font pack");
    c.SetPackManager(&packs);Check(c.LoadFont(name),"load synthetic font");
    Check(c.SetText("2","AA\nAA",10,0xffffff,30,
        {{"align","center"},{"outline","1"},{"outlinecolor","0x000000"}}),"rasterize aligned outlined multiline text");
    c.SetProps("2",{{"left","0"},{"top","0"}});c.Draw();
    auto left=At(2,4), edge=At(8,4), fill=At(10,4), second=At(10,14);
    Check(left[0]==0 && left[2]==255,"center alignment retains left margin");
    Check(edge[0]==0 && edge[2]==0,"outline darkens the border");
    Check(fill[0]==255 && fill[2]==255,"glyph uses em-sized fill");
    Check(second[0]==255 && second[2]==255,"explicit newline places a second line");
    Check(c.SetText("2","A\nA",10,0xffffff,30,
        {{"rubysize","6"},{"spacemiddle","-3"},{"spacebottom","-1"},
         {"left","2"},{"top","1"}}), "reserve ruby row before line spacing");
    c.SetProps("2",{{"left","3"},{"top","1"}});c.Draw();
    auto upper=At(7,7), gap=At(7,14), lower=At(7,19);
    Check(upper[0]==255 && lower[0]==255 && gap[0]==0 && gap[2]==255,
          "ruby spacing leaves a gap and text origin composes with layer translation");
    Check(c.SetText("2","",10,0xffffff,30), "clear empty message");c.Draw();
    Check(At(7,7)[0]==0 && At(7,7)[2]==255, "empty print removes previous glyph pixels");
    artc::Audio message_audio;
    message_audio.Init(&packs);
    artc::LuaEngine messages;
    artc::Ini ini;
    Check(messages.Init(&packs,ini,"android",32,32,&c,&message_audio), "message engine init");
    Check(messages.DoString("e:tag{'chgmsg',id='2'}; e:tag{'font',face='font.ttf',size=10,width=30}; "
        "e:tag{'print',data='A'}; e:tag{'/chgmsg'}; e:tag{'chgmsg',id='4'}; "
        "e:tag{'font',face='font.ttf',size=10,width=30}; e:tag{'print',data='A'}; "
        "e:tag{'chgmsg',id='2'}; e:tag{'print',data='A'}; e:tag{'/chgmsg'}; e:tag{'lydel',id='4'}",
        "append pages"), "selecting a message layer preserves its page");
    c.SetProps("2",{{"left","0"},{"top","0"}}); c.Draw();
    Check(At(9,4)[0]==255, "appended second glyph survives switching message layers");
    Check(messages.DoString("e:tag{'chgmsg',id='2'}; e:tag{'rp'}; e:tag{'print',data='A'}", "clear page"),
          "rp clears the selected page"); c.Draw();
    Check(At(9,4)[0]==0 && At(3,4)[0]==255, "rp replaces the old page instead of appending");
    Check(messages.DoString("e:tag{'lydel',id='2'}; e:tag{'chgmsg',id='2'}; e:tag{'print',data='A'}",
        "recreate message"), "delete message layer"); c.Draw();
    Check(At(9,4)[0]==0, "deleting a layer clears its saved page");
    c.DeleteLayer("2");
    {
        artc::LuaEngine nested_messages;
        Check(nested_messages.Init(&packs,ini,"android",32,32,&c), "nested messages init");
        Check(nested_messages.DoString(R"(
            e:tag{'chgmsg',id='dialogue',layered=0}
            e:tag{'font',face='font.ttf',size=10,width=30}
            e:tag{'print',data='A'}
            e:tag{'chgmsg',id='icon',layered=1}
            e:tag{'font',size=10,width=30,left=20}
            e:tag{'print',data='A'}
            e:tag{'/chgmsg'}
            assert(e:var('s.current_message_layer')=='dialogue')
            e:tag{'print',data='A'}
        )", "nested message pixels"), "nested icon restores the scenario message and its font");
        c.Draw();
        Check(At(3,4)[0]==255 && At(9,4)[0]==255 && At(23,4)[0]==0,
              "non-layered message renders above the scene; layered icon retains ID order");
        Check(nested_messages.DoString("e:tag{'chgmsg',id='dialogue',layered=1}; e:tag{'/chgmsg'}",
                                      "change message layering"), "change existing message layering");
        c.Draw();
        Check(At(3,4)[0]==0 && At(9,4)[0]==0, "layered message returns to scene ID order");
        c.DeleteLayer("dialogue"); c.DeleteLayer("icon");
    }
    c.DeleteTweens("1"); c.SetProps("1",{{"alpha","255"}});
    c.Update(3000);
    c.SetTextTween("2",{{"mode","init"},{"type","in"}});
    c.SetTextTween("2",{{"mode","add"},{"type","in"},{"param","alpha"},
                         {"delay","100"},{"time","100"},{"diff","-255"}});
    Check(c.SetText("2","AA",10,0xffffff,30),"rasterize animated text"); c.Draw();
    Check(At(3,4)[0]==0 && At(9,4)[0]==0,"character fade starts transparent");
    c.Update(3050); c.Draw();
    Check(At(3,4)[0]>=125 && At(3,4)[0]<=130 && At(9,4)[0]==0,
          "first glyph fades while the next glyph waits");
    c.Update(3100); c.Draw();
    Check(At(3,4)[0]==255 && At(9,4)[0]==0,"glyph delay uses characters, independent of layout");
    Check(c.PendingTextMs(3100)==100 && c.FinishText(3100),"finish remaining character animation");
    c.Draw(); Check(At(9,4)[0]==255 && c.PendingTextMs(3100)==0,"finish reveals the complete page");
    Check(c.SetText("2","AAA",10,0xffffff,30),"append to animated page"); c.Draw();
    Check(At(3,4)[0]==255 && At(9,4)[0]==255 && At(15,4)[0]==0,
          "appending preserves revealed glyphs and animates only new text");
    c.Update(3200); c.Draw();
    Check(At(15,4)[0]==255,"appended character completes");
    c.SetProps("2",{{"visible","0"}});
    Check(c.PendingTextMs(3100)==0,"hidden text does not block a visible page");
    c.DeleteLayer("2");
    c.Update(messages.NowMs());
    Check(messages.DoString("e:tag{'chgmsg',id='2'}; e:tag{'rp'}; "
        "e:tag{'scetween',type='in',mode='init'}; "
        "e:tag{'scetween',type='in',mode='add',param='alpha',delay=10000,time=10000,diff=-255}; "
        "e:tag{'print',data='AA'}; e:tag{'@'}; assert(e:getScriptWaitReason().textTween)",
        "typewriter wait"), "script observes pending character animation");
    messages.ClickAt(31,31); messages.RunEnterFrame();
    Check(messages.IsWaiting() && c.PendingTextMs(messages.NowMs())==0,
          "first click completes text and preserves the page wait");
    messages.ClickAt(31,31); messages.RunEnterFrame();
    Check(!messages.IsWaiting(),"second click advances the completed page");
    Check(messages.DoString("e:tag{'rp'}; e:tag{'print',data='AA'}; "
        "e:tag{'automode',allow=1}; e:tag{'exec',command='automode',mode=1}; "
        "e:tag{'var',name='s.automodewait',data=0}; e:tag{'@'}", "auto text wait"),
        "start auto while text is revealing");
    Check(messages.IsWaiting(),"auto waits for text even with zero reading interval");
    c.FinishText(messages.NowMs());
    Check(!messages.IsWaiting(),"auto continues after the complete page is visible");
    c.DeleteLayer("2");
    Check(c.SetText("2","AAAAA",10,0xffffff,30,{{"rubysize","6"}},{{3,2,"AAAA"}}),
          "layout longer ruby as an unbreakable block"); c.Draw();
    const auto& ruby_layer=c.Layers().back();
    Check(ruby_layer.glyphs.size()==9,"ruby adds glyphs without replacing its base");
    const auto& rg=ruby_layer.glyphs;
    Check(rg[3].y>rg[2].y && rg[4].y==rg[3].y,
          "base text and its longer reading wrap together");
    Check(rg[5].y<rg[3].y && rg[5].start_ms==rg[3].start_ms,
          "ruby is placed above the base and shares its character timing");
    int ruby_coverage=0;
    for(int yy=int(rg[5].y);yy<rg[5].y+rg[5].h;++yy)
        for(int xx=int(rg[5].x);xx<rg[5].x+rg[5].w;++xx)
            ruby_coverage=std::max(ruby_coverage,int(At(xx,yy)[0]));
    Check(ruby_coverage>128,"ruby actually produces visible pixels");
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"Aé",10,0xffffff,30,{{"rubysize","6"}},{{1,2,"A"}}),
          "ruby range uses UTF-8 byte boundaries");
    Check(c.Layers().back().glyphs.size()==3,"multibyte base character occupies one glyph");
    c.DeleteLayer("2");
    Check(messages.DoString("e:tag{'chgmsg',id='2'}; e:tag{'rp'}; e:tag{'font',rubysize=6}; "
        "e:tag{'ruby',text='AAAA'}; e:tag{'print',data='AA'}; e:tag{'/ruby'}",
        "ruby tags"), "ruby tags collect base text and render at close");
    Check(c.Layers().back().glyphs.size()==6,"ruby tags retain both the base and annotation");
    Check(messages.DoString("e:tag{'rp'}; e:tag{'print',data='A'}", "clear ruby"), "clear ruby page");
    Check(c.Layers().back().glyphs.size()==1,"rp also clears ruby ranges");
    c.DeleteLayer("2");
    Check(c.SetText("2","AA",10,0xffffff,100),"measure line-break fixture");
    const auto advance=c.Layers().back().glyphs[1].x-c.Layers().back().glyphs[0].x;
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"AAAA。A",10,0xffffff,4*advance,{{"prohibit","0"}}),
          "layout without line-break restrictions");
    Check(c.Layers().back().glyphs[4].y>c.Layers().back().glyphs[3].y,
          "disabled prohibition retains character wrapping");
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"AAAA。A",10,0xffffff,4*advance,{{"prohibit","1"}}),
          "layout prohibited line-start punctuation");
    const auto closing=c.Layers().back().glyphs;
    Check(closing[3].y>closing[2].y && closing[4].y==closing[3].y,
          "closing punctuation carries its preceding character to the next line");
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"AAAA。A",10,0xffffff,4*advance,{{"prohibit","1"},{"hung","1"}}),
          "layout hanging punctuation");
    const auto hanging=c.Layers().back().glyphs;
    Check(hanging[4].y==hanging[3].y && hanging[4].x>=4*advance && hanging[5].y>hanging[4].y,
          "hanging full stop stays on the filled line without pulling the next letter along");
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"AAA（AA",10,0xffffff,4*advance,{{"prohibit","1"}}),
          "layout prohibited line-end bracket");
    const auto opening=c.Layers().back().glyphs;
    Check(opening[3].y>opening[2].y && opening[4].y==opening[3].y,
          "opening bracket wraps with the next character");
    c.DeleteLayer("2");
    Check(c.SetText("2",u8"A\n。",10,0xffffff,4*advance,{{"prohibit","1"},{"hung","1"}}),
          "layout explicit newline before punctuation");
    Check(c.Layers().back().glyphs[1].y>c.Layers().back().glyphs[0].y,
          "explicit newline overrides automatic line-break rules");
    c.DeleteLayer("2");
    // [glyph homing=1] — the click-wait glyph layer parks at the end-of-text
    // pen and follows it onto the last line as the page grows.
    const auto find_layer=[&](const std::string& id)->const artc::Layer* {
        for (const auto& l : c.Layers()) if (l.id==id) return &l;
        return nullptr;
    };
    Check(c.SetText("2","AAAA",10,0xffffff,100),"layout glyph-homing fixture");
    Check(c.HomeLayerToTextPen("2","1.90"),"home glyph layer to the text end");
    Check(find_layer("1.90") && find_layer("1.90")->x==4*advance && find_layer("1.90")->y==0,
          "homing places the glyph at the end of the single line");
    Check(c.SetText("2","AA\nAA",10,0xffffff,100),"grow the homing fixture page");
    Check(c.HomeLayerToTextPen("2","1.90"),"rehome after the page grows");
    Check(find_layer("1.90")->x==2*advance &&
          find_layer("1.90")->y==find_layer("2")->glyphs[2].y-find_layer("2")->glyphs[0].y,
          "homing follows the pen onto the last line");
    Check(!c.HomeLayerToTextPen("missing","1.90"),"homing needs the text layer");
    c.DeleteLayer("2"); c.DeleteLayer("1.90");
    Check(messages.DoString(R"(
        e:tag{'chgmsg',id='adv'}
        e:tag{'font',face='font.ttf',size=10,width=100}
        e:tag{'glyph'}
        e:tag{'glyph',layer='1.90',homing='1'}
        e:tag{'print',data='AAAA'}
        e:tag{'/chgmsg'}
    )", "glyph homing script"), "glyph tag homes through the script path");
    Check(find_layer("1.90") && find_layer("1.90")->x==4*advance && find_layer("1.90")->y==0,
          "printing homes the glyph layer to the scenario text end");
    Check(messages.DoString("e:tag{'chgmsg',id='adv'}; e:tag{'rp'}; e:tag{'/chgmsg'}",
        "clear scripted page"), "rp rehomes the glyph to the text origin");
    Check(find_layer("1.90")->x==0 && find_layer("1.90")->y==0,
          "cleared page returns the glyph to the pen origin");
    Check(messages.DoString("e:tag{'chgmsg',id='adv'}; e:tag{'glyph',layer='1.91',homing='0'}; "
        "e:tag{'print',data='AAAA'}; e:tag{'/chgmsg'}", "fixed glyph"),
        "homing=0 keeps the scripted glyph position");
    Check(!find_layer("1.91"), "homing=0 never materializes or moves the glyph layer");
    c.DeleteLayer("adv"); c.DeleteLayer("1.90");
    c.SetProps("3",{{"left","4"},{"top","3"},{"xscale","200"},{"yscale","200"}});
    Check(c.LoadImage("3.1","small.tga"),"load first expression");
    c.SetProps("3.1",{{"left","5"},{"top","4"}});
    Check(c.LoadImage("3.1","large.tga"),"replace expression with larger bounding box");
    c.SetProps("3.1",{{"left","3"},{"top","2"}});c.Draw();
    Check(At(24,15)[0]==255 && At(27,20)[2]==255,
          "replacement adopts new natural size and composes its offset with parent scale");
    c.SetProps("3.1",{{"clip","2,1,3,2"}});
    Check(c.LoadImage("3.1","small.tga"),"replace cropped expression");c.Draw();
    const auto info=c.GetLayerInfo("3.1");
    Check(info.width==4 && info.height==2,"new surface discards old crop dimensions");
    const auto& face=c.Layers().back();
    Check(face.u0==0 && face.v0==0 && face.u1==1 && face.v1==1,
          "new surface discards old crop UVs");
    Check(At(16,9)[0]==255 && At(18,9)[2]==255,
          "smaller replacement retains layer position and parent transform");
    Check(!c.LoadImage("3.1","missing.png"),"missing replacement fails");
    Check(c.GetLayerInfo("3.1").width==4,"failed replacement keeps existing surface");
    c.DeleteLayer("3");
    Check(c.LoadImage("9.1","small.tga"),"load transform fixture");
    c.SetProps("9",{{"left","16"},{"top","10"},{"rotate","90"},
                    {"xscale","200"},{"yscale","100"}});
    c.SetProps("9.1",{{"left","2"},{"top","1"}}); c.Draw();
    Check(At(14,18)[0]==255 && At(20,12)[2]==255,
          "rotated and scaled parent moves the child pixels");
    Check(c.HitLayer(14,18)=="9.1" && c.HitLayer(20,12)!="9.1",
          "hit testing follows the same rotated child as rendering");
    c.SetProps("9",{{"left","16"},{"top","16"},{"rotate","45"},
                    {"zoom","100"}});
    c.SetProps("9.1",{{"left","0"},{"top","0"},{"w","8"},{"h","8"}});
    Check(c.HitLayer(16,20)=="9.1" && c.HitLayer(11,17)!="9.1",
          "a point in the rotated bounding box but outside the quad does not hit");
    const auto corner_hits=c.HitLayers(11,17);
    Check(std::find(corner_hits.begin(),corner_hits.end(),"9.1")==corner_hits.end(),
          "layer-event hit lists also use the inverse transform");
    c.DeleteLayer("9");
    const uint8_t stripes[]={255,0,0,255, 0,255,0,255};
    Check(c.SetPixels("9.1",stripes,2,1),"load asymmetric mirror fixture");
    c.SetProps("9",{{"left","10"},{"top","10"},{"anchorx","4"},
                    {"anchory","2"},{"reversex","1"}});
    c.SetProps("9.1",{{"w","8"},{"h","4"}}); c.Draw();
    Check(At(11,11)[1]>220 && At(16,11)[0]>220,
          "reversex mirrors child pixels around the parent anchor");
    Check(c.HitLayer(11,11)=="9.1","mirrored content remains clickable");
    c.SetProps("9",{{"reversex","0"},{"reversey","1"},{"rotate","90"}}); c.Draw();
    Check(At(13,9)[0]>220 && At(13,15)[1]>220,
          "vertical reversal composes with anchored clockwise rotation");
    c.SetProps("9",{{"zoom","150"},{"xscale","200"}});
    const auto& group=c.Layers().back();
    Check(group.id=="9" && group.sx==2 && group.sy==1.5,
          "explicit axis scale takes precedence over uniform zoom");
    c.SetProps("9",{{"xscale","0"}});
    Check(c.HitLayer(14,12)!="9.1","collapsed transforms cannot intercept input");
    c.DeleteLayer("9");
    Check(c.SetText("9.1","A",10,0xffffff,10),"load rotated text fixture");
    c.SetProps("9.1",{{"left","0"},{"top","0"}});
    c.SetProps("9",{{"left","20"},{"top","10"},{"rotate","90"}}); c.Draw();
    Check(At(16,13)[0]==255 && At(23,14)[2]==255,
          "glyph batches use the same parent rotation as image quads");
    c.DeleteLayer("9");
    Check(c.LoadImage("9.1","small.tga"),"load rotational tween fixture");
    c.SetProps("9",{{"left","16"},{"top","16"},{"rotate","0"}});
    c.SetProps("9.1",{{"left","4"}});
    c.AddTween("9",{{"rotate","0,180"},{"time","100"},{"yoyo","1"}},4000);
    c.Update(4050); c.Draw();
    Check(At(15,22)[0]==255 && c.HitLayer(15,22)=="9.1",
          "rotation tween draws and hits its clockwise midpoint");
    c.Update(4100); c.Draw();
    Check(At(10,15)[0]==255,"rotation tween reaches its half-turn");
    c.Update(4200); c.Draw();
    Check(At(22,17)[0]==255 && c.PendingAnimationMs(4200)==0,
          "yoyo returns the whole child tree to its original pose");
    c.BeginTweenSet();
    c.AddTween("9",{{"param","rotate"},{"to","90"},{"time","100"}},4300);
    c.AddTween("9",{{"param","rotate"},{"to","180"},{"time","100"}},4300);
    c.EndTweenSet(4300); c.Update(4500); c.Draw();
    Check(At(10,15)[0]==255,"sequential rotations retain their implicit starting angles");
    c.DeleteLayer("9");
    Check(c.LoadImage("9.1","small.tga"),"load transformed draggable fixture");
    c.SetProps("9",{{"left","16"},{"top","10"},{"rotate","90"},
                    {"xscale","200"},{"yscale","100"}});
    c.SetProps("9.1",{{"left","2"},{"top","1"},{"draggable","1"},
                      {"dragarea","0,0,4,3"}});
    messages.BeginDrag(14,18);
    Check(messages.DragActive(),"grab a draggable child in its rotated position");
    messages.DragMove(13,20);
    Check(c.GetLayerInfo("9.1").left==3 && c.GetLayerInfo("9.1").top==2,
          "drag displacement is inverse-rotated and inverse-scaled into parent coordinates");
    messages.DragMove(0,40);
    Check(c.GetLayerInfo("9.1").left==4 && c.GetLayerInfo("9.1").top==3,
          "transformed drag bounds remain local to the parent");
    c.SetProps("9",{{"xscale","0"}}); messages.DragMove(10,10);
    Check(c.GetLayerInfo("9.1").left==4,"collapsed parent ignores subsequent drag motion");
    messages.EndDrag(); c.DeleteLayer("9");
#if defined(ARTC_TEST_MOVIE)
    c.DeleteLayer("3");
    artc::Audio movie_audio; movie_audio.Init(&packs);
    artc::VideoPlayer player(c,movie_audio);
    Check(player.Start(movie,"9",false,true,1000,1000),"start fullscreen movie"); c.Draw();
    Check(Pixel()[0]>220 && Pixel()[2]<30,"movie frame renders above the scene");
    player.Update(1600); c.Draw();
    Check(!player.Active() && Pixel()[2]==255,"movie end restores the underlying scene");
    Check(player.Start(movie,"8",true,false,1000,2000),"start looping layer movie");
    player.Update(2600); player.Update(2800);
    Check(player.Active(),"layer movie loops without ending the player"); player.Stop();
    struct ShortMovieAudio : artc::Audio {
        double position=0;
        bool playing=true;
        bool PlayStream(const std::string&,std::unique_ptr<artc::PcmStream>,int) override { return true; }
        double PlaybackMs(const std::string&) const override { return position; }
        bool IsPlaying(const std::string&) const override { return playing; }
    } short_audio;
    artc::VideoPlayer short_track(c,short_audio);
    Check(short_track.Start(movie,"9",false,true,1000,3000),"start movie with a shorter audio track");
    short_audio.position=200; short_track.Update(3500);
    Check(short_track.Active(),"speaker position controls video despite startup latency");
    short_audio.playing=false; short_track.Update(3600);
    Check(short_track.Active(),"audio drain continues from presented position without a jump");
    short_track.Update(4000); c.Draw();
    Check(!short_track.Active() && Pixel()[2]==255,
          "a stopped audio clock cannot strand the remaining video or its script wait");
    Check(messages.DoString("e:tag{'video',file='movie.mp4',skip=0}; assert(e:getScriptWaitReason().video)",
        "video wait"),"video tag establishes a script wait");
    messages.ClickAt(31,31); messages.RunEnterFrame();
    Check(messages.IsWaiting(),"unskippable movie consumes taps without advancing the scenario");
    Check(messages.DoString("e:tag{'video',file='movie.mp4',skip=2}; e:tag{'keyconfig',role=1,keys='27'}",
        "movie cancel role"),"configure movie cancel key");
    messages.ClickAt(31,31); messages.RunEnterFrame();
    Check(messages.IsWaiting(),"cancel role excludes ordinary pointer taps");
    messages.PushKeyDown(27); messages.RunEnterFrame(); messages.EndFrame();
    Check(!messages.IsWaiting(),"configured cancel key stops the movie and releases the wait");
    Check(messages.DoString("e:tag{'video',file='movie.mp4',skip=1}; e:overrideKey{key=1,status=0}",
        "movie key override"),"start a skippable movie with pointer input suppressed");
    messages.ClickAt(31,31); messages.RunEnterFrame(); messages.EndFrame();
    Check(messages.IsWaiting(),"suppressed pointer input does not cancel a skippable movie");
    Check(messages.DoString("e:overrideKey{key=1,status=-1}","restore movie input"),"restore physical pointer input");
    messages.ClickAt(31,31); messages.RunEnterFrame(); messages.EndFrame();
    Check(!messages.IsWaiting(),"restored pointer input can cancel the movie");
#endif
    c.ReleaseGl();c.Init(32,32);
    Check(c.SetPixels("1",blue,1,1),"shader background");c.SetProps("1",{{"w","32"},{"h","32"}});
    const uint8_t green[4]={0,255,0,255};
    Check(c.SetPixels("9.1",red,1,1) && c.SetPixels("9.2",green,1,1),"shader foreground parts");
    c.SetProps("9.1",{{"w","32"},{"h","32"}});
    c.SetProps("9.2",{{"left","8"},{"top","8"},{"w","16"},{"h","16"}});
    c.SetProps("9",{{"intermediate_render","2"},{"alpha","128"}});c.Draw();
    p=Pixel();Check(abs(p[1]-128)<=1 && abs(p[2]-127)<=1 && p[0]==0,
        "group opacity applies once after overlapping body/face parts");
    p=At(2,2);Check(abs(p[0]-128)<=1 && p[1]==0,"group preserves uncovered body pixels");
    c.SetProps("9",{{"negative","1"}});c.Draw();p=Pixel();
    Check(abs(p[0]-128)<=1 && p[1]==0 && p[2]==255,"negative filter applies to the composed group");
    c.SetProps("9",{{"negative","0"},{"grayscale","1"},{"alpha","255"}});c.Draw();p=Pixel();
    Check(abs(p[0]-150)<=1 && p[0]==p[1] && p[1]==p[2],"native grayscale luminance");
    // E4: fractional grayscale blends toward the native luma, and MODULATE2X
    // colours arrive as straight multiply factors (0x80 neutral).
    c.SetProps("9",{{"grayscale","0.5"}});c.Draw();p=Pixel();
    Check(abs(p[0]-75)<=2 && abs(p[1]-202)<=2 && abs(p[2]-75)<=2,
        "fractional grayscale blends toward the native luma");
    c.SetProps("9",{{"grayscale","0"},{"colormultiply","0x808080"}});c.Draw();p=Pixel();
    Check(p[0]==0 && abs(p[1]-128)<=1 && p[2]==0,
        "colormultiply applies the straight half-intensity factor");
    c.SetProps("9",{{"colormultiply","0xFFFFFF"}});c.Draw();
    // E9: native E-mote blend modes (bm). Multiply keeps the destination
    // alpha; reverse-subtract computes dst - premultiplied src.
    {
        const uint8_t half[4]={128,128,128,255};
        Check(c.SetPixels("31",half,1,1),"blend part");
        c.SetProps("31",{{"w","32"},{"h","32"},{"layermode","multiply"}});c.Draw();
        p=At(2,2);Check(abs(p[0]-128)<=1 && p[1]==0 && p[2]==0 && p[3]==255,
            "multiply blend scales the red body and keeps its alpha");
        const uint8_t white[4]={255,255,255,255};
        Check(c.SetPixels("31",white,1,1),"subtract part");
        c.SetProps("31",{{"layermode","subtract"}});c.Draw();
        p=At(2,2);Check(p[0]==0 && p[1]==0 && p[2]==0,"subtract blend clamps dst minus src");
        c.DeleteLayer("31");c.Draw();
    }
    // General affine is a local opt-in, shared by pixels, inverse rectangle
    // queries and ordinary (not mesh-deformed) stage-mask sources.
    {
        artc::Compositor affine;affine.Init(32,32);
        const uint8_t asymmetric[]={255,0,0,255, 0,255,0,255,
                                    0,0,255,255, 255,255,0,255};
        Check(affine.SetPixels("a",asymmetric,2,2),"upload asymmetric affine quad");
        glBindTexture(GL_TEXTURE_2D,affine.Layers().front().texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        Check(affine.SetLayerAffine("a",8,0,4,8,2,2),"set sheared quad affine");
        affine.Draw();
        Check(At(7,5)[0]>240 && At(7,5)[1]<10 && At(7,5)[2]<10 &&
              At(15,5)[1]>240 && At(15,5)[0]<10 &&
              At(11,13)[2]>240 && At(11,13)[0]<10 &&
              At(19,13)[0]>240 && At(19,13)[1]>240 && At(19,13)[2]<10,
              "affine quad preserves asymmetric UV pixels and top-down direction");
        Check(affine.HitLayers(8,6)==std::vector<std::string>{"a"} &&
              affine.HitLayers(3,16).empty(),"HitLayers inverts the same sheared quad as GL drawing");
        Check(affine.SetLayerAffine("a",-8,0,4,8,18,2),"set negative determinant affine");
        affine.Draw();
        Check(At(15,5)[0]>240 && At(15,5)[1]<10 && At(7,5)[1]>240 &&
              affine.HitLayer(16,6)=="a","reflected affine keeps UV direction and inverse hits");
        Check(affine.SetLayerAffine("a",0,0,4,8,2,2),"accept singular affine for GL");
        affine.Draw();
        Check(At(7,5)[0]==0 && affine.HitLayers(4,6).empty(),
              "zero first column produces no area, not a spurious fallback quad");
        affine.DeleteLayer("a");
        const uint8_t alpha_source[]={255,255,255,255, 255,255,255,255,
                                     255,255,255,0, 255,255,255,0};
        Check(affine.SetPixels("mask",alpha_source,2,2),"upload asymmetric affine mask source");
        glBindTexture(GL_TEXTURE_2D,affine.Layers().front().texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        Check(affine.SetLayerAffine("mask",8,0,4,8,2,2),"set mask source affine");
        const auto mask=affine.RenderStageMask(303,{"mask"});
        Check(mask!=0,"render ordinary affine stage-mask source");affine.DeleteLayer("mask");
        Check(affine.SetPixels("paint",red,1,1),"upload affine mask consumer");
        affine.SetProps("paint",{{"w","32"},{"h","32"}});
        affine.SetLayerStageMask("paint",mask);affine.Draw();
        Check(At(7,5)[0]>240 && At(15,5)[0]>240 && At(11,13)[0]==0 &&
              At(3,5)[0]==0 && At(25,5)[0]==0,
              "ordinary affine mask source shares geometry and asymmetric top-down alpha direction");
        affine.SetProps("paint",{{"intermediate_render","2"}});affine.Draw();
        Check(At(7,5)[0]>240 && At(11,13)[0]==0,
              "affine source stage mask stays top-down through an intermediate consumer");
        affine.ReleaseGl();
    }
    {
        using namespace emote_fixture;
        artc::Compositor affine;affine.Init(32,32);
        auto doc=Scene();
        auto& icon=doc.root.object["source"].object["images"].object["icon"].object["face"];
        const auto offset=doc.resources[icon.object["pixel"].resource].first;
        const uint8_t asymmetric[]={255,0,0,255, 0,255,0,255, 0,0,255,255, 255,255,0,255};
        std::copy(std::begin(asymmetric),std::end(asymmetric),doc.bytes.begin()+offset);
        auto& layers=doc.root.object["object"].object["actor"].object["motion"].object["idle"].object["layer"];
        layers=A({Node(0,"affine art",A({Key(0,2,O({{"src",S("src/images/face")},
            {"angle",N(45)},{"zx",N(.5)}}))}))});
        auto model=std::make_shared<artc::EmoteModel>();std::string error;
        Check(model->Load(doc,error),error.c_str());artc::EmoteScene scene;
        Check(scene.Load(model,error),error.c_str());
        affine.SetProps("scene",{{"left","16"},{"top","16"},{"zoom","800"}});
        Check(scene.Render(affine,"scene",0,{},error),error.c_str());
        for(const auto& l:affine.Layers())if(l.texture) {
            glBindTexture(GL_TEXTURE_2D,l.texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
        affine.Draw();
        Check(At(16,10)[0]>240 && At(16,10)[1]<10 && At(18,16)[1]>240 && At(18,16)[0]<10 &&
              At(13,16)[2]>240 && At(13,16)[0]<10 && At(16,21)[0]>240 && At(16,21)[1]>240,
              "E-mote installs full 45-degree nonuniform affine with asymmetric pixel UVs");
        Check(affine.HitLayer(18,16)=="scene.000001.000000" && scene.HitTest(affine,"scene","affine art",18,16),
              "E-mote geometric hits and compositor inverse hits agree with GL affine art");
        scene.Remove(affine,"scene");affine.DeleteLayer("scene");
        layers=A({Node(1,"shape",A({Key(0,2,O({{"src",S("shape/rect")},
            {"coord",A({N(20),N(20)})},{"opa",N(128)}}))}),A({
            Node(0,"shape child",A({Key(0,2,O({{"src",S("src/images/face")},
                {"coord",A({N(-1),N(-1)})}}))}))}))});
        model=std::make_shared<artc::EmoteModel>();Check(model->Load(doc,error),error.c_str());
        Check(scene.Load(model,error),error.c_str());Check(scene.Render(affine,"scene",0,{},error),error.c_str());
        affine.Draw();
        const auto child=At(18,18);
        Check(child[0]+child[1]+child[2]>110 && child[0]+child[1]+child[2]<200 &&
              affine.HitLayer(18.5,18.5)=="scene.000001.000001.000000" &&
              scene.HitTest(affine,"scene","shape",20,20),
              "shape geometry leaf leaves child absolute placement and opacity applied exactly once in GL");
        affine.ReleaseGl();
    }
    // E9: stencil composite stage mask — the mask layer's alpha clips the
    // masked part (RenderStageMask + SetLayerStageMask).
    {
        const uint8_t mask_white[4]={255,255,255,255};
        Check(c.SetPixels("51",mask_white,1,1),"mask source part");
        c.SetProps("51",{{"left","4"},{"top","4"},{"w","8"},{"h","8"}});c.Draw();
        const uint32_t stage_mask=c.RenderStageMask(1,{"51"});
        Check(stage_mask!=0,"render stage mask");
        Check(c.SetPixels("52",red,1,1),"masked part");
        c.SetProps("52",{{"w","32"},{"h","32"}});
        c.SetLayerStageMask("52",stage_mask);
        c.Draw();
        Check(At(6,6)[0]>200 && At(20,20)[0]==0,
            "stage mask keeps the part inside the mask only");
        c.SetLayerStageMask("52",0);
        c.DeleteLayer("51");c.DeleteLayer("52");c.Draw();
    }
    {
        artc::Compositor masks;
        const uint8_t white[4]={255,255,255,255};
        for(int cycle=0;cycle<2;++cycle) {
            masks.Init(32,32);
            // Reuse the keys after ReleaseGl, but move both coverage regions
            // so stale mask contents cannot accidentally satisfy the checks.
            const int ax=cycle?20:4, bx=cycle?4:20;
            Check(masks.SetPixels("51",white,1,1),"independent mask A source");
            masks.SetProps("51",{{"left",std::to_string(ax)},{"top","4"},{"w","8"},{"h","24"}});
            const uint32_t mask_a=masks.RenderStageMask(101,{"51"});
            Check(mask_a!=0,"render independent mask A");
            masks.DeleteLayer("51");
            Check(masks.SetPixels("61",red,1,1),"independent mask A consumer");
            masks.SetProps("61",{{"w","32"},{"h","32"}});
            masks.SetLayerStageMask("61",mask_a);masks.Draw();
            Check(At(ax+2,6)[0]==255 && At(bx+2,22)[0]==0,
                "mask A initially covers only its own region");

            Check(masks.SetPixels("52",white,1,1),"independent mask B source");
            masks.SetProps("52",{{"left",std::to_string(bx)},{"top","4"},{"w","8"},{"h","24"}});
            const uint32_t mask_b=masks.RenderStageMask(202,{"52"});
            Check(mask_b!=0 && mask_b!=mask_a,"different mask keys own distinct textures");
            masks.DeleteLayer("52");
            Check(masks.SetPixels("62",green,1,1),"independent mask B consumer");
            masks.SetProps("62",{{"w","32"},{"h","32"}});
            masks.SetLayerStageMask("62",mask_b);masks.Draw();
            const auto a=At(ax+2,6), b=At(bx+2,22), gap=At(16,16);
            Check(a[0]==255 && a[1]==0 && b[0]==0 && b[1]==255 &&
                  gap[0]==0 && gap[1]==0,
                "creating mask B preserves mask A coverage for distinct same-frame consumers");
            Check(At(ax+2,22)[0]==255 && At(ax+2,22)[1]==0 &&
                  At(bx+2,6)[0]==0 && At(bx+2,6)[1]==255,
                "rebuilt masks never retain coverage from before GL release");
            masks.ReleaseGl();
            Check(!glIsTexture(mask_a) && !glIsTexture(mask_b),
                "GL release deletes both independently keyed mask textures");
        }
        // Unlike the symmetric coverage above, this catches stage-space Y
        // inversion without an already-red background hiding the error.
        masks.Init(32,32);
        Check(masks.SetPixels("51",white,1,1),"asymmetric stage mask source");
        masks.SetProps("51",{{"left","4"},{"top","4"},{"w","8"},{"h","8"}});
        const uint32_t top_mask=masks.RenderStageMask(101,{"51"});
        Check(top_mask!=0,"render asymmetric stage mask");masks.DeleteLayer("51");
        Check(masks.SetPixels("61",red,1,1),"asymmetric stage mask consumer");
        masks.SetProps("61",{{"w","32"},{"h","32"}});
        masks.SetLayerStageMask("61",top_mask);masks.Draw();
        const auto top=At(6,6), bottom=At(6,25);
        if(top[0]!=255 || bottom[0]!=0)
            std::cerr << "stage mask top red=" << int(top[0]) << " bottom red=" << int(bottom[0]) << '\n';
        Check(top[0]==255 && bottom[0]==0,"stage mask coverage keeps top-down stage coordinates");
        masks.SetProps("61",{{"intermediate_render","2"}});masks.Draw();
        Check(At(6,6)[0]==255 && At(6,25)[0]==0,
            "explicit intermediate stage mask keeps top-down stage coordinates");
        // Nest the masked layer so End composites into a top-down parent FBO,
        // rather than directly into the ordinary scene FBO.
        masks.RenameLayer("61","60.1");
        masks.SetProps("60",{{"intermediate_render","2"}});masks.Draw();
        Check(At(6,6)[0]==255 && At(6,25)[0]==0 && At(20,6)[0]==0,
            "nested intermediate stage mask keeps top-down stage coordinates");
        masks.ReleaseGl();
    }
    // Warped E9 mask sources use the same triangle positions and explicit UVs
    // as the ordinary leaf pass, not the layer's rectangular texture bounds.
    {
        artc::Compositor warped;warped.Init(32,32);
        const uint8_t alpha_uv[]={255,255,255,0, 255,255,255,255,
                                  255,255,255,255, 255,255,255,128};
        const std::vector<float> triangle={0,0,1,0, 24,0,0,0, 0,24,1,1};
        Check(warped.SetPixels("shape",alpha_uv,2,2),"upload warped mask asymmetric alpha");
        warped.SetProps("shape",{{"left","4"},{"top","4"},{"w","24"},{"h","24"}});
        warped.SetLayerMesh("shape",triangle);
        glBindTexture(GL_TEXTURE_2D,warped.Layers().front().texture);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        warped.Draw();
        Check(At(7,7)[0]==255 && At(21,7)[0]==0 && abs(At(7,21)[0]-128)<=1 && At(23,23)[0]==0,
            "synthetic mesh leaf establishes nonrectangular geometry and asymmetric UV alpha");
        // Keep the existing source-alpha policy: positive layer opacity gates
        // eligibility but is not multiplied into the sampled texture alpha.
        warped.SetProps("shape",{{"alpha","64"},{"intermediate_render","2"},{"clip","0,0,1,1"}});
        const auto mask=warped.RenderStageMask(401,{"shape"});
        Check(mask!=0,"render warped triangle stage mask");
        // The source's own mask/effect must not be recursively evaluated, even
        // when it names the very target being rebuilt (texture feedback).
        warped.SetLayerStageMask("shape",mask);
        Check(warped.RenderStageMask(401,{"shape"})==mask,"rebuild warped mask without source-effect feedback");
        warped.DeleteLayer("shape");
        Check(warped.SetPixels("paint",red,1,1),"upload warped mask consumer");
        warped.SetProps("paint",{{"w","32"},{"h","32"}});
        warped.SetLayerStageMask("paint",mask);warped.Draw();
        auto check_warped=[&]() {
            Check(At(7,7)[0]==255 && At(21,7)[0]==0 && abs(At(7,21)[0]-128)<=1 &&
                  At(23,23)[0]==0 && At(7,28)[0]==0,
                "warped stage-mask consumer matches triangle geometry, explicit UV alpha and top-down stage Y");
        };
        check_warped();
        warped.SetProps("paint",{{"intermediate_render","2"}});warped.Draw();check_warped();
        warped.RenameLayer("paint","group.paint");
        warped.SetProps("group",{{"intermediate_render","2"}});warped.Draw();check_warped();
        warped.ReleaseGl();
        Check(!glIsTexture(mask),"release warped mask GL target");
    }
    {
        artc::Compositor warped;warped.Init(32,32);
        const uint8_t alpha_uv[]={255,255,255,0, 255,255,255,255,
                                  255,255,255,255, 255,255,255,128};
        Check(warped.SetPixels("parent.shape",alpha_uv,2,2),"upload full-affine warped mask source");
        warped.SetProps("parent.shape",{{"w","24"},{"h","24"}});
        warped.SetLayerMesh("parent.shape",{0,0,1,0, 24,0,0,0, 0,24,1,1});
        Check(warped.SetLayerAffine("parent.shape",1,.25f,.5f,.75f,2,2),"install warped mask full affine");
        warped.SetProps("parent",{{"alpha","128"}});
        Check(warped.SetLayerAffine("parent",1,.25f,0,1,1,1),"install warped mask ancestor affine");
        for(const auto& layer:warped.Layers())if(layer.texture) {
            glBindTexture(GL_TEXTURE_2D,layer.texture);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
        const auto mask=warped.RenderStageMask(402,{"parent.shape"});
        Check(mask!=0,"render full-affine warped mask");
        warped.DeleteLayer("parent.shape");warped.DeleteLayer("parent");
        Check(warped.SetPixels("paint",red,1,1),"upload full-affine warped mask consumer");
        warped.SetProps("paint",{{"w","32"},{"h","32"}});
        warped.SetLayerStageMask("paint",mask);warped.Draw();
        Check(At(8,8)[0]==255 && At(22,15)[0]==0 && abs(At(15,20)[0]-128)<=1 && At(30,28)[0]==0,
            "warped mask composes full local and ancestor affine without reducing to a quad or TRS");
        warped.ReleaseGl();
    }
    {
        artc::Compositor warped;
        const uint8_t white[4]={255,255,255,255};
        for(int cycle=0;cycle<2;++cycle) {
            warped.Init(32,32);
            const int y=cycle?20:2;
            uint32_t targets[2]={};
            for(int i=0;i<2;++i) {
                const int x=i?20:2;
                Check(warped.SetPixels("shape",white,1,1),"upload independently keyed warped mask source");
                warped.SetProps("shape",{{"left",std::to_string(x)},{"top",std::to_string(y)},{"w","8"},{"h","8"}});
                warped.SetLayerMesh("shape",{0,0,0,0, 8,0,1,0, 0,8,0,1});
                targets[i]=warped.RenderStageMask(410+i,{"shape"});
                Check(targets[i]!=0 && (!i || targets[i]!=targets[0]),"warped mask keys have independent targets");
                warped.DeleteLayer("shape");
                const std::string id=i?"paintB":"paintA";
                Check(warped.SetPixels(id,i?green:red,1,1),"upload independently keyed warped mask consumer");
                warped.SetProps(id,{{"w","32"},{"h","32"}});
                warped.SetLayerStageMask(id,targets[i]);
            }
            warped.Draw();
            Check(At(3,y+1)[0]==255 && At(3,y+1)[1]==0 && At(21,y+1)[1]==255 && At(21,y+1)[0]==0 &&
                  At(8,y+6)[0]==0 && At(26,y+6)[1]==0 && At(3,cycle?3:21)[0]==0,
                "independent warped mask consumers preserve triangle coverage across key creation and GL rebuild");
            warped.ReleaseGl();
            Check(!glIsTexture(targets[0]) && !glIsTexture(targets[1]),"release every independently keyed warped mask");
        }
    }
    {
        artc::Compositor bounds;bounds.Init(32,32);
        const uint8_t white[4]={255,255,255,255};
        Check(bounds.SetPixels("shape",white,1,1),"upload mask mesh boundary source");
        bounds.SetProps("shape",{{"left","4"},{"top","4"},{"w","8"},{"h","8"}});
        Check(bounds.SetPixels("paint",red,1,1),"upload mask mesh boundary consumer");
        bounds.SetProps("paint",{{"w","32"},{"h","32"}});
        const auto target=bounds.RenderStageMask(420,{"shape"});
        Check(target!=0,"create boundary mask target");
        bounds.SetLayerStageMask("paint",target);
        auto check_mesh=[&](const std::vector<float>& mesh,bool rendered,int inside,int corner,const char* why) {
            bounds.SetProps("shape",{{"visible","1"}});
            bounds.SetLayerMesh("shape",mesh);
            Check(bounds.RenderStageMask(420,{"shape"})==(rendered?target:0),why);
            // Hide only after capturing alpha so the raw source never paints
            // over the consumer, and invalid vertices never enter Draw().
            bounds.SetProps("shape",{{"visible","0"}});bounds.Draw();
            Check(At(5,5)[0]==inside && At(10,10)[0]==corner,why);
        };
        check_mesh({0,0,0,0, 8,0,1,0, 0,8,0,1},true,255,0,"boundary mesh triangle clips its far corner");
        check_mesh({},true,255,255,"empty SetLayerMesh restores the quad mask");
        check_mesh({0,0,0},true,255,255,"misaligned mesh size follows the leaf quad fallback");
        check_mesh({0,0,0,0},true,0,0,"incomplete triangle list draws no accidental quad");
        check_mesh({0,0,0,0, 8,0,1,0, 0,8,0,1, 8,8,1,1},true,255,0,
            "complete xyuv vertices with an incomplete trailing triangle match GL_TRIANGLES");
        check_mesh({0,0,0,0, INFINITY,0,1,0, 0,8,0,1},false,0,0,
            "nonfinite mask mesh coordinates safely leave cleared coverage");
        check_mesh({0,0,NAN,0, 8,0,1,0, 0,8,0,1},false,0,0,
            "nonfinite mask mesh UVs safely leave cleared coverage");
        bounds.ReleaseGl();
    }
    c.SetProps("9",{{"grayscale","0"}});
    Check(messages.DoString("e:tag{'lyshader',id='tint',file='filter.glsl'};e:tag{'lyshader',id='uv',file='uv.glsl'}",
        "load game shader"),"lyshader loads source through the resource resolver");
    c.SetProps("9",{{"shader","tint"},{"shaderconstant","red,green,blue"},{"red","1"},{"green","0.5"},{"blue","1"}});
    c.Draw();p=Pixel();Check(abs(p[1]-128)<=1,"custom GLSL executes on the actual layer subtree");
    // Invisible layers can supply a custom sampler without entering the scene.
    Check(c.SetPixels("99",blue,1,1),"custom shader auxiliary texture");c.SetProps("99",{{"visible","0"}});
    c.SetProps("9",{{"shadertexture","textureUser"},{"textureUser","99"},{"userMix","1"}});c.Draw();
    Check(Pixel()[2]==255 && Pixel()[1]==0,"shadertexture resolves an invisible layer");
    c.SetProps("9",{{"shader","uv"}});c.Draw();
    Check(At(2,2)[1]<30 && At(2,29)[1]>220,"native shader coordinates are top-down after offscreen composition");
    c.SetProps("9",{{"shader",""},{"negative","1"}});
    c.SetProps("9.2",{{"intermediate_render","2"},{"negative","1"}});c.Draw();
    p=Pixel();Check(p[1]==255 && p[0]==0 && p[2]==0,"nested shader passes apply child then parent effects");
    // Half-transparent screen blending must attenuate the destination by the
    // premultiplied source, rather than its full straight-alpha color.
    c.DeleteLayer("9.2");c.SetProps("9",{{"negative","0"},{"layermode","screen"},{"alpha","128"}});
    const uint8_t grey[4]={128,128,128,255};c.SetPixels("1",grey,1,1);c.SetProps("1",{{"w","32"},{"h","32"}});
    c.Draw();p=Pixel();Check(abs(p[0]-192)<=1 && p[1]==128 && p[2]==128,"screen respects group opacity");
    c.SetProps("9",{{"layermode","add"}});c.Draw();p=Pixel();
    Check(p[0]==255 && p[1]==128 && p[2]==128,"additive group composition");
    c.SetProps("9",{{"alpha","0"}});c.Draw();p=Pixel();
    Check(p[0]==128 && p[1]==128 && p[2]==128,"zero-opacity shader group produces no pixels");
    c.DeleteLayer("9");c.SetPixels("1",blue,1,1);c.SetProps("1",{{"w","32"},{"h","32"}});
    c.SetPixels("9.1",red,1,1);c.SetProps("9.1",{{"w","16"},{"h","16"}});
    c.SetProps("9",{{"left","8"},{"top","8"},{"intermediate_render","1"},
        {"intermediate_render_mask","mask.tga"},{"alpha","128"}});c.Draw();
    Check(abs(At(9,9)[0]-128)<=1 && abs(At(9,13)[0]-64)<=2 && At(14,9)[0]==0 && At(18,9)[0]==0,
        "local grayscale mask multiplies alpha once and never extends its border pixels");
    c.SetProps("9",{{"clip","0,0,8,4"}});c.Draw();
    Check(abs(At(9,9)[0]-128)<=1 && At(9,13)[0]==0,"intermediate clip intersects the mask");
    c.SetProps("9",{{"left","16"},{"rotate","90"}});c.Draw();
    Check(abs(At(15,9)[0]-128)<=1 && At(15,14)[0]==0 && At(11,9)[0]==0,
        "mask and clip follow the effect group's rotated local coordinates");
    c.SetProps("9",{{"intermediate_render_mask",""},{"clip","0,0,0,0"}});c.Draw();
    Check(At(15,9)[0]==0,"zero-sized group clip renders no content");
    c.SetProps("9",{{"intermediate_render","0"},{"alpha","255"}});c.Draw();
    Check(At(15,9)[0]==255,"disabling the intermediate clip restores its children");
    c.ReleaseGl();c.Init(32,32);
    c.SetPixels("9.1",red,1,1);c.SetProps("9.1",{{"w","16"},{"h","16"}});
    c.SetProps("9",{{"intermediate_render","2"},{"intermediate_render_mask","mask.tga"}});c.Draw();
    Check(At(1,1)[0]==255 && At(6,1)[0]==0,"mask textures reload after GL resource release");
    Check(messages.DoString("e:setMagicPath{'coverage','mask.tga'};e:tag{'lyprop',id='9',intermediate_render_mask=':coverage'}",
        "mask path alias"),"mask tag resolves the same resource aliases as image loading");c.Draw();
    Check(At(1,1)[0]==255 && At(6,1)[0]==0 && c.Layers().back().effect.mask=="mask.tga","resolved mask retains its clipping result");
    c.ReleaseGl();c.Init(32,32);
    {
        artc::Compositor order;order.Init(32,32);order.SetPackManager(&packs);
        auto fill=[&](const std::string& id,const uint8_t* color) {
            Check(order.SetPixels(id,color,1,1),"synthetic ordering texture");
            order.SetProps(id,{{"w","32"},{"h","32"}});
        };
        const std::array<std::string,3> ids={"30.000001.000000",
            "30.000001.000001.000000","30.000001.000000.5"};
        // The previous pairwise comparator formed A<X, X<B, B<A for these
        // mixed flagged/unflagged layers. Every insertion permutation must now
        // have the same deterministic result (including the ordinary member).
        std::array<int,3> permutation={0,1,2};std::string expected;
        do {
            order.DeleteLayer("30");
            for(int i:permutation) {
                fill(ids[size_t(i)],i==0?red:(i==1?green:blue));
                if(i<2) {
                    order.SetLayerEmoteTree(ids[size_t(i)],true);
                    order.SetLayerEmoteOrder(ids[size_t(i)],"30",-1);
                }
            }
            const auto description=order.DescribeDrawList(20);
            if(expected.empty())expected=description;
            Check(description==expected,"mixed E-mote/ordinary ordering is insertion-independent across all six permutations");
            const auto a=description.find(ids[0]+"["),b=description.find(ids[1]+"["),x=description.find(ids[2]+"[");
            Check(x<b && b<a,"virtual postorder keys replace the mixed-layer comparison cycle");
            order.Draw();Check(Pixel()[0]==255 && Pixel()[1]==0,
                "same-node child components are behind the node's own art");
        } while(std::next_permutation(permutation.begin(),permutation.end()));
        artc::CompositorOrderRegression::AllocationFree(order);
        // Runtime Z is the primary local channel; explicit paint is retained
        // only as a same-Z extension. Finite keys keep strict ordering valid.
        const auto before_invalid=order.DescribeDrawList(20);const auto invalid_rev=order.Revision();
        Check(!order.SetLayerEmoteOrder(ids[0],"30",0,INFINITY) &&
              !order.SetLayerEmoteOrder(ids[0],"30",0,std::nan("")) &&
              order.Revision()==invalid_rev && order.DescribeDrawList(20)==before_invalid,
              "non-finite effective Z rejects atomically without invalidating total-order keys");
        order.SetLayerEmoteOrder(ids[0],"30",0,-5);order.SetLayerPaint(ids[0],100);
        order.SetLayerEmoteOrder(ids[1],"30",1,5);order.SetLayerPaint(ids[1],-100);
        auto z_description=order.DescribeDrawList(20);
        Check(z_description.find(ids[0]+"[")<z_description.find(ids[1]+"["),
              "effective Z precedes explicit paint and compatibility rank within a boundary");
        const auto changed_rev=order.Revision();
        order.SetLayerEmoteOrder(ids[0],"30",0,-5);
        Check(order.Revision()==changed_rev,"publishing identical effective Z does not bump revision");
        order.SetLayerEmoteOrder(ids[0],"30",0,8);
        Check(order.Revision()>changed_rev,"a Z-only change bumps redraw revision");
        artc::CompositorOrderRegression::AllocationFree(order);
        order.DeleteLayer("30");
        fill("1",blue);
        for(const std::string owner:{"30","40"}) {
            fill(owner+".000001.000000",red);
            fill(owner+".000002.000000",green);
            order.SetLayerEmoteOrder(owner+".000001.000000",owner,0);
            order.SetLayerEmoteOrder(owner+".000002.000000",owner,1);
            order.SetLayerPaint(owner+".000002.000000",-3);
        }
        auto description=order.DescribeDrawList(20);
        Check(description.find(" 1[")<description.find(" 30.000002.000000[") &&
              description.find(" 30.000002.000000[")<description.find(" 30.000001.000000[") &&
              description.find(" 30.000001.000000[")<description.find(" 40.000002.000000[") &&
              description.find(" 40.000002.000000[")<description.find(" 40.000001.000000["),
              "background then two owner blocks, with local paint but no cross-owner interleaving");
        // Use an uncovered negative-paint part: it must not disappear behind
        // the background or escape its owner/ancestor effects.
        order.DeleteLayer("40");order.DeleteLayer("30.000001.000000");
        order.SetProps("30",{{"grayscale","1"}});order.Draw();
        Check(abs(Pixel()[0]-150)<=1 && Pixel()[0]==Pixel()[1] && Pixel()[1]==Pixel()[2],
            "negative-paint content remains above background and inside parent grayscale");
        Check(order.LoadShader("tint","filter.glsl"),"load owner sorting shader fixture");
        order.SetProps("30",{{"shader","tint"},{"red","1"},{"green","0.5"},{"blue","1"}});order.Draw();
        Check(Pixel()[0]==0 && abs(Pixel()[1]-128)<=1 && Pixel()[2]==0,
            "negative-paint content remains inside the parent custom shader");
        order.SetProps("30",{{"shader",""}});
        order.SetProps("30",{{"intermediate_render","2"},{"alpha","128"},{"clip","0,0,16,16"}});order.Draw();
        Check(abs(At(4,4)[0]-75)<=2 && abs(At(4,4)[2]-202)<=2 && At(24,24)[2]==255 && At(24,24)[0]==0,
            "negative-paint content retains parent intermediate opacity and local clip coverage");
        // A nested real effect is an indivisible boundary, even if one child
        // asks to paint before both its group and its sibling group.
        order.SetProps("30",{{"grayscale","0"},{"alpha","255"},{"clip","0,0,32,32"}});
        order.DeleteLayer("30.000002.000000");
        order.SetProps("30.1",{{"grayscale","1"}});
        fill("30.1.1",green);order.SetLayerEmoteOrder("30.1.1","30",2);order.SetLayerPaint("30.1.1",-9);
        fill("30.2",red);order.SetLayerEmoteOrder("30.2","30",3);order.Draw();
        description=order.DescribeDrawList(20);
        Check(description.find(" 30.2[")<description.find(" 30.1.1[") && abs(Pixel()[0]-150)<=1 && Pixel()[0]==Pixel()[1],
            "nested effect descendants stay with their group instead of sorting out through negative paint");
        artc::CompositorOrderRegression::AllocationFree(order);
        // Repeat the owner/effect invariants with actual Z instead of paint.
        order.DeleteLayer("30");order.DeleteLayer("40");
        fill("30.1",green);order.SetLayerEmoteOrder("30.1","30",0,-100);
        fill("40.1",red);order.SetLayerEmoteOrder("40.1","40",0,-1000);
        order.Draw();description=order.DescribeDrawList(20);
        Check(Pixel()[0]==255 && description.find(" 30.1[")<description.find(" 40.1["),
              "two owner-local Z domains remain external-ID blocks instead of globally interleaving");
        order.DeleteLayer("40");order.SetProps("30",{{"grayscale","1"}});order.Draw();
        Check(abs(Pixel()[0]-150)<=1 && Pixel()[0]==Pixel()[1] && Pixel()[1]==Pixel()[2],
              "negative Z stays above ordinary background and inside owner grayscale");
        order.SetProps("30",{{"shader","tint"},{"red","1"},{"green","0.5"},{"blue","1"}});order.Draw();
        Check(Pixel()[0]==0 && abs(Pixel()[1]-128)<=1 && Pixel()[2]==0,
              "negative Z remains in owner custom shader");
        order.SetProps("30",{{"shader",""},{"intermediate_render","2"},{"alpha","128"},{"clip","0,0,16,16"}});order.Draw();
        Check(abs(At(4,4)[0]-75)<=2 && abs(At(4,4)[2]-202)<=2 && At(24,24)[2]==255 && At(24,24)[0]==0,
              "negative Z retains owner intermediate opacity and local clip");
        order.DeleteLayer("30");
        order.SetProps("30.1",{{"grayscale","1"}});
        fill("30.1.1",green);order.SetLayerEmoteOrder("30.1.1","30",0,-100);
        fill("30.2",red);order.SetLayerEmoteOrder("30.2","30",1,-5);
        order.Draw();description=order.DescribeDrawList(20);
        Check(description.find(" 30.2[")<description.find(" 30.1.1[") && abs(Pixel()[0]-150)<=1,
              "negative descendant Z cannot leave the enclosing indivisible nested effect group");
        artc::CompositorOrderRegression::AllocationFree(order);
        // An effect may also live above the scene owner. Both owner blocks and
        // ordinary siblings must still be enclosed by that pre-existing group.
        order.DeleteLayer("30");
        order.SetProps("30",{{"grayscale","1"},{"intermediate_render","2"}});
        fill("30.10.1",green);order.SetLayerEmoteOrder("30.10.1","30.10",0);order.SetLayerPaint("30.10.1",-9);
        fill("30.20.1",red);order.SetLayerEmoteOrder("30.20.1","30.20",0);order.SetLayerPaint("30.20.1",-2);
        order.Draw();Check(abs(Pixel()[0]-76)<=1 && Pixel()[0]==Pixel()[1] && Pixel()[1]==Pixel()[2],
            "common ancestor effect encloses both external owner blocks with negative paint");
        order.SetLayerEmoteOrder("30.10.1","30.10",0,1000);
        order.SetLayerEmoteOrder("30.20.1","30.20",0,-1000);
        order.Draw();Check(abs(Pixel()[0]-76)<=1 && Pixel()[0]==Pixel()[1] && Pixel()[1]==Pixel()[2],
            "common ancestor effect still encloses both owner blocks with opposite runtime Z");
        artc::CompositorOrderRegression::AllocationFree(order);
        // Ordinary UI keeps ID ordering and message_overlay takes precedence;
        // stale/local paint metadata cannot reorder layers outside an owner.
        order.DeleteLayer("30");order.DeleteLayer("1");
        fill("500.d.1",red);fill("500.0",green);fill("1.80",blue);
        order.SetLayerPaint("500.0",-9);order.SetLayerPaint("1.80",9);
        description=order.DescribeDrawList(20);
        Check(description.find(" 1.80[")<description.find(" 500.0[") &&
              description.find(" 500.0[")<description.find(" 500.d.1["),"ordinary UI preserves its original ID order");
        order.SetMessageLayered("1.80",false);order.Draw();
        Check(Pixel()[2]==255,"message overlay remains above numeric UI regardless of paint");
        order.ReleaseGl();
    }
    {
        artc::Compositor order;order.Init(32,32);
        auto fill=[&](const std::string& id,const uint8_t* color) {
            Check(order.SetPixels(id,color,1,1),"owner lifecycle synthetic texture");
            order.SetProps(id,{{"w","32"},{"h","32"}});
        };
        auto layer=[&](const std::string& id)->const artc::Layer& {
            for(const auto& l:order.Layers())if(l.id==id)return l;
            Check(false,"owner lifecycle resident layer exists");std::abort();
        };
        fill("30.1",red);fill("30.2",green);
        Check(order.SetLayerEmoteOrder("30.1","30",1) && order.SetLayerEmoteOrder("30.2","30",0),
            "register disjoint owner members");
        order.SetLayerEmoteOrder("30.1","30",1,8);
        order.SetLayerEmoteOrder("30.2","30",0,-8);
        Check(order.RenameLayer("30","40"),"rename complete owner subtree");
        Check(layer("40.1").effect.emote_owner=="40" && layer("40.2").effect.emote_owner=="40" &&
              layer("40.1").effect.emote_order==1 && layer("40.2").effect.emote_order==0 &&
              layer("40.1").effect.emote_z==8 && layer("40.2").effect.emote_z==-8,
            "whole-owner rename maps owner metadata while retaining local ranks");
        order.Draw();Check(Pixel()[0]==255 && order.HitLayer(8,8)=="40.1" && order.HitLayers(8,8).front()=="40.1",
            "renamed owner preserves pixels and both hit-query priority orders");
        fill("30.1",blue);fill("30.2",green);order.SetLayerPaint("30.1",9);
        auto description=order.DescribeDrawList(20);
        Check(description.find(" 30.1[")<description.find(" 30.2["),"old owner ID reuse has no ghost registration affecting ordinary layers");
        Check(order.SetLayerEmoteOrder("30.1","30.1",0),"old owner ID can be reused by a nested disjoint owner");
        order.DeleteLayer("30");
        Check(order.RenameLayer("40.1","50.1"),"move a member out of its owner");
        Check(layer("50.1").effect.emote_owner.empty() && layer("50.1").effect.emote_order==-1 &&
               !layer("50.1").effect.emote_tree && layer("50.1").effect.emote_z==0,
               "moved-out member is unregistered and loses foreign Z");
        order.SetLayerPaint("40.2",-9);
        Check(order.RenameLayer("40.2","50.2"),"move last member out of its owner");
        Check(layer("50.2").effect.emote_owner.empty() && layer("50.2").effect.emote_order==-1 &&
               layer("50.2").effect.paint==0 && layer("50.2").effect.emote_z==0,
               "move out clears source owner paint, rank and Z");
        fill("40.1",green);Check(order.SetLayerEmoteOrder("40.1","40.1",0),
            "moving final member removes source owner registration");
        order.DeleteLayer("40");
        fill("60.1",green);Check(order.SetLayerEmoteOrder("60.1","60",0),"register destination owner");
        Check(order.SetLayerEmoteOrder("50.1","50",7),"register reparent source");order.SetLayerPaint("50.1",-9);
        Check(order.RenameLayer("50.1","60.2"),"reparent single member into different owner");
        Check(layer("60.2").effect.emote_owner.empty() && layer("60.2").effect.emote_order==-1 &&
              layer("60.2").effect.paint==0,"destination must not inherit foreign owner rank or paint");
        Check(order.SetLayerEmoteOrder("60.2","60",-1),"reparented member can explicitly register destination fallback rank");
        order.Draw();Check(Pixel()[0]==255 && order.HitLayer(8,8)=="60.2","unranked reparent uses destination fallback order");
        Check(order.RenameLayer("60","40.1") && layer("40.1.1").effect.emote_owner=="40.1" &&
              layer("40.1.2").effect.emote_owner=="40.1",
            "owner can move into an unregistered destination subtree");
        // Reset for atomic owner-conflict rejection, including equal-owner merge.
        order.DeleteLayer("40");order.DeleteLayer("60");order.DeleteLayer("50");
        fill("30.1",red);fill("40.1",green);
        Check(order.SetLayerEmoteOrder("30.1","30",0) && order.SetLayerEmoteOrder("40.1","40",0),"two lifecycle owners");
        const auto before=order.DescribeDrawList(20);const auto rev=order.Revision();
        Check(!order.RenameLayer("30","40.2") && !order.RenameLayer("30","40") &&
              order.DescribeDrawList(20)==before && order.Revision()==rev,
            "whole-owner rename rejects nested-owner and equal-owner merges atomically");
        order.DeleteLayer("30");order.DeleteLayer("40");
        fill("30.9",red);fill("30.1.a",green);fill("30.1.b",blue);
        Check(order.SetLayerEmoteOrder("30.9","30",1),"register outer owner");
        const auto outer_revision=order.Revision();
        Check(!order.SetLayerEmoteOrder("30.1.a","30.1",0) &&
              !order.SetLayerEmoteOrder("30.1.b","30.1",2) && order.Revision()==outer_revision &&
              layer("30.1.a").effect.emote_owner.empty() && layer("30.1.b").effect.emote_owner.empty(),
            "nested owner registrations are rejected without mutation, not merged across local ranks");
        Check(order.SetLayerEmoteOrder("30.9","",99),"explicit unregister outer owner");
        Check(order.SetLayerEmoteOrder("30.1.a","30.1",0) && order.SetLayerEmoteOrder("30.1.b","30.1",2),
            "nested owner may register after outer owner is unregistered");
        const auto nested_revision=order.Revision();
        Check(!order.SetLayerEmoteOrder("30.9","30",1) && order.Revision()==nested_revision,
            "overlap is rejected in reverse registration order too");
        description=order.DescribeDrawList(20);
        Check(description.find(" 30.1.a[")<description.find(" 30.1.b[") &&
              description.find(" 30.1.b[")<description.find(" 30.9["),"accepted nested scene stays contiguous beside ordinary outer sibling");
        order.DeleteLayer("30.1");
        Check(order.SetLayerEmoteOrder("30.9","30",1),"deleting last nested registration releases overlapping owner namespace");
        order.DeleteLayer("30");fill("30.1",red);fill("30.2",green);order.SetLayerPaint("30.1",9);
        description=order.DescribeDrawList(20);
        Check(description.find(" 30.1[")<description.find(" 30.2["),"delete then ordinary ID reuse has no ghost owner");
        Check(!order.SetLayerEmoteOrder("missing.1","missing",0) &&
              !order.SetLayerEmoteOrder("30.1","40",0),"absent and nonmember registration rejected");
        order.DeleteLayer("30");
        fill("30.1",red);fill("40.1",green);fill("30.2",blue);
        Check(order.SetLayerEmoteOrder("30.1","30",0) && order.SetLayerEmoteOrder("40.1","40",0),
            "ordinary attachment migration source and destination owners");
        order.SetLayerPaint("30.2",-9);order.SetLayerEmoteTree("30.2",true);
        Check(order.RenameLayer("30.2","40.2") && layer("40.2").effect.emote_owner.empty() &&
              layer("40.2").effect.paint==0 && layer("40.2").effect.emote_order==-1 &&
              !layer("40.2").effect.emote_tree,
            "ordinary cross-owner attachment clears source-local hint, rank and marker");
        order.Draw();Check(Pixel()[2]==255 && order.HitLayer(8,8)=="40.2",
            "ordinary cross-owner attachment does not bring negative paint into destination");
        order.SetLayerPaint("40.2",-9);order.SetLayerEmoteTree("40.2",true);
        Check(order.RenameLayer("40.2","50.2") && layer("50.2").effect.paint==0 &&
              layer("50.2").effect.emote_order==-1 && !layer("50.2").effect.emote_tree,
            "ordinary attachment moved out of all owners clears source-local metadata");
        Check(order.RenameLayer("50.2","40.2"),"return ordinary attachment to destination owner");
        order.SetLayerPaint("40.2",-9);
        Check(order.RenameLayer("40.2","40.3") && layer("40.3").effect.paint==-9,
            "ordinary attachment rename within one owner retains its local hint");
        order.Draw();Check(Pixel()[1]==255 && order.HitLayer(8,8)=="40.1",
            "same-owner ordinary attachment keeps negative paint behind registered art");
        Check(order.RenameLayer("40","60") && layer("60.3").effect.emote_owner.empty() &&
              layer("60.3").effect.paint==-9 && layer("60.1").effect.emote_owner=="60",
            "whole-owner rename preserves ordinary attachment hints alongside mapped registrations");
        order.Draw();Check(Pixel()[1]==255 && order.HitLayer(8,8)=="60.1",
            "whole-owner move preserves ordinary attachment relative paint order");
        order.DeleteLayer("60");order.DeleteLayer("30");
        fill("30.1",red);fill("30.2",green);order.SetLayerPaint("30.1",9);
        // Defensive source filtering: malformed legacy/stale metadata outside
        // its declared subtree must not establish a phantom owner or rank.
        fill("70.1",blue);
        auto& stale=const_cast<artc::Layer&>(layer("70.1"));
        stale.effect.emote_owner="30";stale.effect.emote_order=0;
        description=order.DescribeDrawList(20);
        Check(description.find(" 30.1[")<description.find(" 30.2[") &&
              order.SetLayerEmoteOrder("30.1","30.1",0),
            "only valid resident registrations may supply owner roots");
        order.DeleteLayer("30");order.DeleteLayer("70");
        fill("20.30.1",red);fill("20.30.2",green);
        Check(order.SetLayerEmoteOrder("20.30.1","20.30",1) &&
              order.SetLayerEmoteOrder("20.30.2","20.30",0) && order.RenameLayer("20","50") &&
              layer("50.30.1").effect.emote_owner=="50.30" &&
              layer("50.30.2").effect.emote_owner=="50.30",
            "renaming an owner's ancestor maps owner metadata as well as layer IDs");
        order.Draw();Check(Pixel()[0]==255 && order.HitLayer(8,8)=="50.30.1",
            "ancestor rename preserves owner-local paint and hit order");
        artc::CompositorOrderRegression::AllocationFree(order);
        order.ReleaseGl();
    }
    {
        using namespace emote_fixture;
        auto doc=Scene();
        auto& motion=doc.root.object["object"].object["actor"].object["motion"].object["idle"];
        motion.object["layer"]=A({
            Node(0,"body",A({Key(0,2,O({{"src",S("src/images/body")},{"coord",A({N(8),N(8),N(0)})}}))})),
            Node(0,"face",A({Key(0,2,O({{"src",S("src/images/face")},{"coord",A({N(8),N(8),N(0)})}}))}))});
        motion.object["priority"]=A({O({{"time",N(0)},{"content",A({N(0),N(1)})}}),
            O({{"time",N(5)},{"content",A({N(1),N(0)})}})});
        auto model=std::make_shared<artc::EmoteModel>();std::string error;
        Check(model->Load(doc,error),error.c_str());artc::EmoteScene scene;
        Check(scene.Load(model,error),error.c_str());
        Check(scene.Render(c,"30",0,{},error),error.c_str());c.Draw();
        Check(At(8,8)[0]==255 && At(8,8)[1]==0,"motion.priority controls actual rendered order, not only Evaluate vector order");
        Check(c.HitLayer(8,8)=="30.000001.000000" && c.HitLayers(8,8).front()=="30.000001.000000",
            "both hit queries follow first priority keyframe's visible top part");
        const auto first=c.DescribeDrawList(20);
        Check(scene.Render(c,"30",5,{},error),error.c_str());c.Draw();
        Check(At(8,8)[0]==0 && At(8,8)[1]==255 && c.DescribeDrawList(20)!=first,
            "priority keyframe changes compositor order without changing original-index layer IDs");
        Check(c.HitLayer(8,8)=="30.000002.000000" && c.HitLayers(8,8).front()=="30.000002.000000",
            "both hit queries follow changed motion.priority instead of original-index ID order");
        Check(c.GetLayerInfo("30.000001.000000").found && c.GetLayerInfo("30.000002.000000").found,
            "priority preserves layer identity across frames");
        c.ReleaseGl();c.Init(32,32);
    }
    {
        using namespace emote_fixture;
        // Stencil masks are resolved per masked part and per owner. Two mask
        // sources share one label (left/right), and one model is installed by
        // two owners with the same label list: each masked part is clipped to
        // ITS nearest source, never to a mask built for a sibling or another
        // owner. Synthetic art only; red body clipped by green face masks.
        auto doc=Scene();
        auto picture=[](const char* label,const char* src,double x,double z,double scale) {
            return Node(0,label,A({Key(0,2,O({{"src",S(src)},{"coord",A({N(x),N(0),N(z)})},
                {"zx",N(scale)},{"zy",N(scale)}}))}));
        };
        auto group=Node(12,"group",A({Key(0,2,O({{"src",S("layout")}})),Key(11,0)}),
            A({picture("left part","src/images/body",-8,5,3),picture("right part","src/images/body",8,5,3)}));
        group.object["stencilType"]=N(4);
        group.object["stencilCompositeMaskLayerList"]=A({S("source")});
        doc.root.object["object"].object["actor"].object["motion"].object["idle"].object["layer"]=
            A({picture("source","src/images/face",-8,0,4),picture("source","src/images/face",8,0,4),group});
        auto model=std::make_shared<artc::EmoteModel>();std::string error;
        Check(model->Load(doc,error),error.c_str());
        artc::EmoteScene upper,lower;
        Check(upper.Load(model,error),error.c_str());Check(lower.Load(model,error),error.c_str());
        c.SetProps("30",{{"left","16"},{"top","9"}});c.SetProps("31",{{"left","16"},{"top","24"}});
        Check(upper.Render(c,"30",0,{},error),error.c_str());
        Check(lower.Render(c,"31",0,{},error),error.c_str());c.Draw();
        const auto red=[&](int x,int y){const auto p=At(x,y);return p[0]>200 && p[1]<40;};
        for(const int cy:{9,24}) {
            Check(red(8,cy) && red(24,cy),
                "each masked part keeps the coverage of its own nearest mask source for every owner");
            // Parts span stage x 2..14 and 18..30; their masks only 4..12 and 20..28.
            Check(!red(2,cy) && !red(13,cy) && !red(19,cy) && !red(29,cy),
                "masked parts are clipped outside their own mask coverage");
        }
        upper.Remove(c,"30");lower.Remove(c,"31");c.DeleteLayer("30");c.DeleteLayer("31");
        c.ReleaseGl();c.Init(32,32);
    }
    {
        using namespace emote_fixture;
        // Entirely synthetic overlapping red/green icons; no asset labels or
        // atlas metadata may stand in for the sampled runtime coord[2].
        auto picture=[](const char* label,const char* src,double z) {
            return Node(0,label,A({Key(0,2,O({{"src",S(src)},
                {"coord",A({N(0),N(0),N(z)})}}))}));
        };
        auto run=[&](artc::PsbValue nodes,bool green_top,const char* message) {
            auto doc=Scene();auto& motion=doc.root.object["object"].object["actor"].object["motion"].object["idle"];
            motion.object["layer"]=std::move(nodes);
            doc.root.object["object"].object["actor"].object["motion"].object["inner"]=O({
                {"layer",A({picture("nested green","src/images/face",-2)})},
                {"lastTime",N(20)},{"loopTime",N(-1)}});
            auto model=std::make_shared<artc::EmoteModel>();std::string error;
            Check(model->Load(doc,error),error.c_str());artc::EmoteScene scene;
            Check(scene.Load(model,error),error.c_str());
            c.SetProps("30",{{"left","8"},{"top","8"}});
            Check(scene.Render(c,"30",0,{},error),error.c_str());c.Draw();
            Check(At(8,8)[green_top?1:0]==255 && At(8,8)[green_top?0:1]==0,message);
            artc::CompositorOrderRegression::AllocationFree(c);
            c.ReleaseGl();c.Init(32,32);
        };
        run(A({picture("green","src/images/face",5),picture("red","src/images/body",-2)}),true,
            "sampled positive/negative Z overrides sibling source-index order on GPU");
        for(double z:{-3.0,3.0,0.0}) {
            auto parent=picture("parent art","src/images/body",4);
            parent.object["children"]=A({picture("child art","src/images/face",z)});
            run(A({parent}),z>0,"same-node parent art and child +/-Z sort by accumulated Z, same Z retains compatibility tie");
        }
        auto high=Node(2,"positive parent",A({Key(0,2,O({{"src",S("layout")},
            {"coord",A({N(0),N(0),N(10)})}}))}),A({picture("green","src/images/face",-1)}));
        auto low=Node(2,"negative parent",A({Key(0,2,O({{"src",S("layout")},
            {"coord",A({N(0),N(0),N(-10)})}}))}),A({picture("red","src/images/body",1)}));
        run(A({high,low}),true,"parent Z accumulation wins across unrelated child branches");
        for(bool split:{false,true}) {
            auto entry=Node(3,"nested entry",A({Key(0,2,O({
                {"src",S(split?"actor":"motion/actor/inner")},{"icon",S("inner")},
                {"coord",A({N(0),N(0),N(6)})}}))}));
            auto parent=Node(2,"outer",A({Key(0,2,O({{"src",S("layout")},
                {"coord",A({N(0),N(0),N(10)})}}))}),A({entry}));
            run(A({parent,picture("red","src/images/body",12)}),true,
                "nested motion GPU order includes both ancestor and attachment Z (10+6-2 > 12)");
        }
        // The label spellings used by the removed hacks are ordinary strings.
        run(A({picture("neutral","src/images/body",0),picture("後髪","src/images/face",0)}),true,
            "authored hair labels do not synthesize paint hints at equal Z");
        run(A({picture("neutral","src/images/body",0),picture("■首","src/images/face",0)}),true,
            "authored neck labels do not synthesize paint hints at equal Z");
        auto hair_group=Node(2,"髪",A({Key(0,2,O({{"src",S("layout")}}))}),
            A({picture("追加パーツ","src/images/face",0)}));
        run(A({picture("neutral","src/images/body",0),hair_group}),true,
            "additional-part labels under hair scopes do not synthesize paint hints");
        // Icon changes must not fetch an atlas zorder, and a HOLD in the middle
        // of playback keeps the last live Z rather than falling back to zero.
        auto doc=Scene();auto& motion=doc.root.object["object"].object["actor"].object["motion"].object["idle"];
        auto animated=Node(0,"animated",A({
            Key(0,3,O({{"src",S("src/images/face")},{"coord",A({N(0),N(0),N(4)})}})),
            Key(4,2,O({{"src",S("src/images/wide")},{"coord",A({N(0),N(0),N(-4)})}})),
            Key(6,0),Key(9,2,O({{"src",S("src/images/face")},{"coord",A({N(0),N(0),N(4)})}}))}));
        motion.object["layer"]=A({animated,picture("red","src/images/body",0)});
        motion.object["lastTime"]=N(20);motion.object["loopTime"]=N(-1);
        auto& icons=doc.root.object["source"].object["images"].object["icon"];
        icons.object["face"].object["metadata"]=O({{"zorder",N(-100)}});
        icons.object["wide"].object["metadata"]=O({{"zorder",N(100)}});
        auto model=std::make_shared<artc::EmoteModel>();std::string error;
        Check(model->Load(doc,error),error.c_str());artc::EmoteScene scene;
        Check(scene.Load(model,error),error.c_str());c.SetProps("30",{{"left","8"},{"top","8"}});
        for(double frame:{0.0,1.0,3.0,4.0,5.0,6.0,8.0,9.0,25.0}) {
            Check(scene.Render(c,"30",frame,{},error),error.c_str());c.Draw();
            const bool green_top=frame<2 || frame>=9;
            Check(At(8,8)[green_top?1:0]==255 && At(8,8)[green_top?0:1]==0,
                "interpolated runtime Z, icon switch and HOLD drive actual pixels without atlas overrides");
            const auto expected=green_top?"30.000001.000000":"30.000002.000000";
            Check(c.HitLayer(8,8)==expected && c.HitLayers(8,8).front()==expected,
                "both production rectangle hit queries track current Z draw order");
            artc::CompositorOrderRegression::AllocationFree(c);
        }
        c.ReleaseGl();c.Init(32,32);
    }
    auto emote=std::make_shared<artc::EmoteModel>();std::string emote_error;
    Check(emote->Load(emote_fixture::Scene(),emote_error),emote_error.c_str());
    artc::EmoteScene emote_scene;Check(emote_scene.Load(emote,emote_error),emote_error.c_str());
    Check(emote_scene.Render(c,"30",0,{},emote_error),emote_error.c_str());c.Draw();
    Check(At(6,8)[0]==255 && At(7,6)[1]==255 && At(10,8)[0]==0,
        "E-mote picture origins align body and child-motion face on the GPU");
    Check(emote_scene.Render(c,"30",5,{},emote_error),emote_error.c_str());c.Draw();
    Check(At(11,8)[0]==255 && At(12,6)[1]==255 && At(7,6)[1]==0,
        "E-mote keyframe advancement moves face and body together");
    Check(emote_scene.Render(c,"30",5,{{"expression",10}},emote_error),emote_error.c_str());c.Draw();
    Check(At(11,6)[1]==255 && At(14,6)[1]==255 && At(15,6)[1]==0,
        "E-mote expression replacement keeps the new image origin and dimensions");
    c.ReleaseGl();c.Init(32,32);
    Check(emote_scene.Render(c,"30",5,{{"expression",10}},emote_error),emote_error.c_str());c.Draw();
    Check(At(11,6)[1]==255,"E-mote scene reuploads textures after GL context resources are discarded");
    artc::SnapshotImage snapshot;
    Check(c.Snapshot(snapshot) && snapshot.width==32 && snapshot.rgba[(6*32+11)*4+1]==255,
        "retained scene capture has top-down image coordinates");
    const auto retained=snapshot.rgba;c.DeleteLayer("30");c.Draw();
    Check(snapshot.rgba==retained,"captured save image does not change when menus replace the scene");
    std::vector<uint8_t> png;Check(snapshot.EncodePng(16,16,png),"encode sized PNG snapshot");
    int png_width=0,png_height=0,channels=0;
    auto* decoded=stbi_load_from_memory(png.data(),int(png.size()),&png_width,&png_height,&channels,4);
    Check(decoded && png_width==16 && png_height==16 && decoded[(3*16+6)*4+1]>200,"PNG round-trip preserves the upper scene and size");
    stbi_image_free(decoded);const auto valid_png=png;
    Check(!snapshot.EncodePng(0,16,png) && !snapshot.EncodePng(8193,1,png) && png==valid_png,"invalid snapshot dimensions preserve output");
    artc::SnapshotImage transparent;transparent.width=2;transparent.height=1;
    transparent.rgba={255,0,0,255,0,0,255,0};Check(transparent.EncodePng(1,1,png),"resize transparent image");
    decoded=stbi_load_from_memory(png.data(),int(png.size()),&png_width,&png_height,&channels,4);
    Check(decoded && decoded[0]==255 && decoded[2]==0 && decoded[3]==128,"thumbnail interpolation cannot leak colors from transparent pixels");
    stbi_image_free(decoded);
    const auto save_directory=path.string()+"-saves";std::filesystem::create_directory(save_directory);
    messages.SetSaveDir(save_directory);
    Check(emote_scene.Render(c,"30",5,{{"expression",10}},emote_error),emote_error.c_str());c.Draw();
    Check(messages.DoString("e:tag{'takess'}","capture save point"),"capture through script tag");
    c.DeleteLayer("30");c.SetPixels("1",blue,1,1);c.SetProps("1",{{"w","32"},{"h","32"}});c.Draw();
    Check(messages.DoString(R"(
        e:tag{'savess',file='scene',width=16,height=16}
        local file=e:var('s.savepath')..'/scene.png'
        assert(e:isFileExists(file) and e:file(file):sub(1,4)=='\137PNG')
        e:tag{'lyc',id='40',file=e:var('s.savepath')..'/scene'}
    )","save thumbnail"),"write the retained scene after the menu changes");
    c.Draw();
    Check(c.GetLayerInfo("40").width==16 && At(6,3)[1]>200,"save page displays the captured scene, not the later blue menu");
    std::ifstream thumbnail(save_directory+"/scene.png",std::ios::binary);
    const std::vector<uint8_t> before((std::istreambuf_iterator<char>(thumbnail)),{});
    Check(messages.DoString("e:tag{'savess',file='scene',width=0,height=16}","reject invalid save size"),"invalid size is handled");
    std::ifstream unchanged(save_directory+"/scene.png",std::ios::binary);
    Check(std::vector<uint8_t>((std::istreambuf_iterator<char>(unchanged)),{})==before,"invalid thumbnail request preserves the previous file");
    Check(messages.DoString("assert(e:file('scene.png'):sub(1,4)=='\\137PNG')","read saved PNG"),"script file reads include authorized saved images");
    {
        // Mac save-load scene shape: 783 layers, ids up to 20 segments deep.
        // Ancestor/owner/id work per Draw must be linear in the layer count,
        // not O(layers*depth) matrix compositions or O(layers^2) id scans.
        c.ReleaseGl();c.Init(32,32);
        const uint8_t px[4]={255,0,0,255};
        size_t layers=0;
        auto build=[&](bool owners) {
            for(int k=0;k<39;++k) {
                std::string id=std::to_string(100+k);const std::string owner=id;
                for(int d=1;d<=20;++d) {
                    if(!owners) {
                        c.SetProps(id,{{"left","1"},{"top","1"}});
                        if(d%5==0){Check(c.SetPixels(id,px,1,1),"perf texture");c.SetProps(id,{{"w","8"},{"h","8"}});}
                        ++layers;
                    } else Check(c.SetLayerEmoteOrder(id,owner,d),"perf owner registration");
                    id+=".1";
                }
            }
        };
        build(false);
        for(int k=0;k<3;++k){c.SetProps(std::to_string(900+k),{});++layers;}
        Check(layers==783 && c.Layers().size()==783,"synthetic 783-layer scene");
        auto cheapest_draw=[&] {
            uint64_t best=~0ull;
            // The 300-frame diagnostic walks every layer once; the cheapest of
            // three consecutive draws excludes it.
            for(int i=0;i<3;++i) {
                const auto before=c.Counters();c.Draw();
                best=std::min(best,c.Counters().transform_applies-before.transform_applies);
            }
            return best;
        };
        c.Draw();
        Check(cheapest_draw()<=layers,"Draw composes each layer's world transform at most once");
        build(true);
        c.Draw();
        const auto before_owned=c.Counters();
        Check(cheapest_draw()<=layers,"Draw with E-mote owners composes each layer at most once");
        const auto after_owned=c.Counters();
        Check(after_owned.index_rebuilds==before_owned.index_rebuilds &&
              after_owned.owner_rebuilds==before_owned.owner_rebuilds &&
              after_owned.index_lookups-before_owned.index_lookups<=3*layers*21,
              "steady-state Draw reuses the id index and owner list");
        c.ReleaseGl();c.Init(32,32);
    }
    {
        artc::Compositor scene;
        scene.Init(32,32);
        scene.SetPackManager(&packs);
        Check(scene.LoadFont(name),"checkpoint scene font");
        Check(scene.LoadImage("bg","small.tga"),"checkpoint scene image");
        scene.SetProps("bg",{{"left","12"},{"top","34"},{"alpha","128"},{"visible","1"}});
        scene.SetMessageLayered("bg",false);
        Check(scene.SetText("adv","restored text",25,0xffffff,720,
                            {{"align","left"},{"outline","1"}}),"checkpoint scene text");
        std::vector<artc::SavedCommand> commands;
        std::string error;
        Check(scene.CaptureScene(commands,error),"capture semantic scene");
        Check(!commands.empty(),"scene emits semantic commands");
        scene.DeleteLayer("bg");
        scene.DeleteLayer("adv");
        Check(scene.RestoreScene(commands,error),"restore semantic scene");
        Check(scene.Layers().size()==2,"restored layer count");
        const auto& bg=scene.Layers().at(0);
        Check(bg.source_file=="small.tga" && bg.x==12 && bg.y==34 &&
              bg.alpha>0.49f && bg.alpha<0.51f && bg.visible && bg.message_overlay,
              "restore image semantics");
        const auto& adv=scene.Layers().at(1);
        Check(adv.text=="restored text" && adv.text_size==25 &&
              adv.text_wrap_width==720 && adv.text_style.at("align")=="left",
              "restore text semantics");

        const auto before=scene.Layers();
        scene.SetLayerMesh("bg",{0,0,0,0, 1,0,1,0, 0,1,1,1});
        Check(!scene.CaptureScene(commands,error),"mesh checkpoint is explicitly unsupported");
        Check(scene.Layers().size()==before.size(),"unsupported capture preserves layers");
        scene.SetLayerMesh("bg",{});
        scene.AddTween("bg",{{"param","left"},{"from","12"},{"to","60"},
                            {"time","1000"},{"delay","100"},{"loop","0"},{"yoyo","-1"}},1000);
        scene.SetAnimeFrame("cursor","init","small.tga",0,-1,{{"clip","0,0,4,4"}},1000);
        scene.SetAnimeFrame("cursor","add","small.tga",500,-1,{{"clip","4,0,4,4"}},1000);
        scene.SetAnimeFrame("cursor","add","small.tga",1000,-1,{{"clip","8,0,4,4"}},1000);
        scene.SetAnimeFrame("cursor","end","",1000,-1,{},1000);
        scene.Update(1250);
        Check(scene.CaptureScene(commands,error),"capture active tween and anime");
        const auto tween=std::find_if(commands.begin(),commands.end(),[](const auto& command) {
            return command.name=="lytween";
        });
        Check(tween!=commands.end() && tween->attrs.at("param")=="left" &&
              std::stod(tween->attrs.at("elapsed"))==250 &&
              tween->attrs.at("repeat")=="-1" && tween->attrs.at("yoyo")=="1",
              "capture tween loop and signed phase");
        const auto anime_end=std::find_if(commands.begin(),commands.end(),[](const auto& command) {
            return command.name=="anime" && command.attrs.at("mode")=="end";
        });
        Check(anime_end!=commands.end() && anime_end->attrs.at("loop")=="-1" &&
              std::stod(anime_end->attrs.at("elapsed"))==250,
              "capture anime loop and phase");
        scene.BeginTweenSet();
        scene.AddTween("bg",{{"param","top"},{"from","34"},{"to","80"},
                            {"time","200"},{"delay","100"}},1250);
        scene.AddTween("bg",{{"param","top"},{"to","20"},{"time","200"}},1250);
        scene.EndTweenSet(1250);
        Check(scene.CaptureScene(commands,error),"capture ordered tween set");
        const auto future=std::find_if(commands.begin(),commands.end(),[](const auto& command) {
            return command.name=="lytween" && command.attrs.at("param")=="top" &&
                   command.attrs.at("to")=="20.000000";
        });
        Check(future!=commands.end() && std::stod(future->attrs.at("elapsed"))==-300,
              "future tween-set segment retains a negative phase");
        scene.BeginTweenSet();
        Check(!scene.CaptureScene(commands,error),"uncommitted tween set is unsupported");
        scene.EndTweenSet(1250);
        scene.DeleteLayer("bg");
        scene.DeleteLayer("cursor");
        Check(scene.RestoreScene(commands,error),"restore animated scene");
        scene.Update(2100);
        const auto& bg_restored=scene.Layers().at(0);
        Check(bg_restored.x>59.9f && bg_restored.x<60.1f,
              "infinite yoyo resumes at the original cycle endpoint");
        scene.Update(1750);
        const auto cursor_it=std::find_if(scene.Layers().begin(),scene.Layers().end(),
                                          [](const auto& layer){return layer.id=="cursor";});
        Check(cursor_it!=scene.Layers().end(),"restored cursor exists");
        const auto& cursor=*cursor_it;
        Check(cursor.source_file=="small.tga" && cursor.tex_w>0 &&
              std::abs(cursor.u0-4.0f/cursor.tex_w)<0.0001f,
              "looping anime advances from the restored phase");
        auto before_animation=scene.Layers();
        auto bad_animation=commands;
        for(auto& command:bad_animation)
            if(command.name=="lytween") command.attrs["elapsed"]="nan";
        Check(!scene.RestoreScene(bad_animation,error),"non-finite tween phase rejected");
        Check(scene.Layers().size()==before_animation.size(),"invalid animation preserves scene");
        bad_animation=commands;
        for(auto& command:bad_animation)
            if(command.name=="anime" && command.attrs.at("mode")=="add")
                command.attrs["file"]="missing.png";
        Check(!scene.RestoreScene(bad_animation,error),"missing anime frame rejected before mutation");
        Check(scene.Layers().size()==before_animation.size(),"missing anime frame preserves scene");
        scene.SetAnimeFrame("finished","init","small.tga",0,1,{{"clip","0,0,4,4"}},2000);
        scene.SetAnimeFrame("finished","add","small.tga",500,1,{{"clip","8,0,4,4"}},2000);
        scene.SetAnimeFrame("finished","end","",500,1,{},2000);
        scene.Update(2500);
        Check(scene.CaptureScene(commands,error),"capture completed finite anime");
        Check(scene.RestoreScene(commands,error),"restore completed finite anime");
        const auto finished_it=std::find_if(scene.Layers().begin(),scene.Layers().end(),
                                            [](const auto& layer){return layer.id=="finished";});
        Check(finished_it!=scene.Layers().end() && finished_it->u0>0,
              "completed finite anime holds its last frame");
        scene.DeleteTweens("bg");
        scene.Draw();
        Check(scene.BeginTransition(0,100,{},0,0,0),"start transition");
        Check(scene.CaptureScene(commands,error),"capture final scene during transition");
        Check(scene.RestoreScene(commands,error),"restore final transition scene");
        Check(!scene.TransitionActive(),"old-frame overlay is not restored");
        const auto before_invalid=scene.Layers();
        auto bad=commands;
        bad.push_back({"lyc",{{"id","missing"},{"file","missing.png"}}});
        Check(!scene.RestoreScene(bad,error),"missing scene resource rejected before mutation");
        Check(scene.Layers().size()==before_invalid.size(),"invalid restore preserves current layers");
        const std::vector<artc::SavedCommand> malformed = {
            {"font",{{"face","font.ttf"},{"size","25"},{"color","0"}}},
            {"font",{{"id","adv"},{"face","font.ttf"},{"size","25"}}},
            {"print",{{"id","adv"},{"data","missing width"}}},
            {"print",{{"id","no-font"},{"data","missing font"},{"width","720"}}},
            {"chgmsg",{{"id","adv"}}},
        };
        for(const auto& command:malformed) {
            auto invalid=commands;
            invalid.push_back(command);
            Check(!scene.RestoreScene(invalid,error),
                  ("malformed scene command rejected without throwing: "+command.name).c_str());
            Check(scene.Layers().size()==before_invalid.size(),
                  "malformed command preserves the current scene");
        }
        scene.ReleaseGl();
    }
    std::filesystem::remove_all(save_directory);
    std::filesystem::remove(path);
    c.Shutdown();
    Check(glGetError()==GL_NO_ERROR, "resource cleanup");
#if defined(ARTC_TEST_CGL)
    CGLSetCurrentContext(nullptr);
#else
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(d,ctx); eglDestroySurface(d,surface); eglTerminate(d);
#endif
    std::cout << "retained framebuffer regression passed\n";
}
