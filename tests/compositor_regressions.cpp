#include "render/compositor.h"
#include "render/layer_shader.h"
#include "render/emote_scene.h"
#include "render/stb_image.h"
#include "emote_scene_fixture.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "render/video_player.h"
#include "audio/audio.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <array>
#include <cstdlib>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <chrono>
#include <algorithm>

static void Check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << " GL=" << glGetError() << '\n'; std::exit(1); }
}
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
        glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,32,32);
        glClearColor(0,0,1,1);glClear(GL_COLOR_BUFFER_BIT);
        Check(shaders.Begin(0,32,32,0,false)!=0,"allocate intermediate layer");
        glClearColor(0.5,0,0,0.5);glClear(GL_COLOR_BUFFER_BIT); // premultiplied red
        Check(shaders.End(0,effect,0,false,1,{}),"apply native GLSL uniforms");
        auto pixel=Pixel();
        Check(abs(pixel[0]-96)<=2 && abs(pixel[1]-16)<=2 && abs(pixel[2]-128)<=2,
              "unpremultiply before custom shader, then composite once");
        // Parameter state belongs to the layer, even when two layers share a program.
        effect.parameters.clear();
        glBindFramebuffer(GL_FRAMEBUFFER,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.Begin(0,32,32,0,false);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.End(0,effect,0,false,1,{});pixel=Pixel();
        Check(pixel[0]==0 && pixel[1]==0,"shader parameters never leak between layers");
        // CPU shader sources survive context/resource recreation, as on native load.
        shaders.ReleaseGl();
        glBindFramebuffer(GL_FRAMEBUFFER,0);glClearColor(0,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        shaders.Begin(0,32,32,0,false);glClearColor(1,0,0,1);glClear(GL_COLOR_BUFFER_BIT);
        effect.Set({{"red","1"}});shaders.End(0,effect,0,false,1,{});
        Check(Pixel()[0]==255,"recompile retained shader after GL release");
        Check(shaders.Load("integers",R"(
            precision mediump float; uniform ivec3 tint; uniform bvec3 mask; uniform mat3 basis;
            uniform sampler2D images[2]; varying vec2 resultCoord1;
            void main(){vec3 c=vec3(tint)/255.0*vec3(mask);
                gl_FragColor=vec4(basis*c*texture2D(images[0],resultCoord1).rgb*texture2D(images[1],resultCoord1).rgb,1.0);}
        )"),"compile integer/boolean vectors and sampler array");
        effect.Set({{"shader","integers"},{"tint","128,64,255"},{"mask","-2,0,1"},{"basis","1,0,0,0,1,0,0,0,1"}});
        shaders.Begin(0,32,32,0,false);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        Check(shaders.End(0,effect,0,false,1,{}),"bind all reflected GLSL uniform types");
        pixel=Pixel();Check(abs(pixel[0]-128)<=1 && pixel[1]==0 && pixel[2]==255 && glGetError()==GL_NO_ERROR,
            "integer vectors and bool normalization reach shader without stale texture-array units");
        GLuint inputs[2];glGenTextures(2,inputs);
        const uint8_t colors[2][4]={{255,128,255,255},{128,255,255,255}};
        for(int n=0;n<2;++n) {
            glBindTexture(GL_TEXTURE_2D,inputs[n]);glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,colors[n]);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        }
        effect.Set({{"images[0]","first"},{"images[1]","second"}});
        shaders.Begin(0,32,32,0,false);glClearColor(0,0,0,0);glClear(GL_COLOR_BUFFER_BIT);
        shaders.End(0,effect,0,false,1,{{"first",inputs[0]},{"second",inputs[1]}});
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
    artc::LuaEngine messages;
    artc::Ini ini;
    Check(messages.Init(&packs,ini,"android",32,32,&c), "message engine init");
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
    std::filesystem::remove_all(save_directory);
    std::filesystem::remove(path);
    c.Shutdown();
    Check(glGetError()==GL_NO_ERROR, "resource cleanup");
    eglMakeCurrent(d,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(d,ctx); eglDestroySurface(d,surface); eglTerminate(d);
    std::cout << "retained framebuffer regression passed\n";
}
