// Font style inheritance through production Lua tags and GLES glyph rendering.
// Uses the project's original rectangular font; no game assets or API stubs.
#include "config/ini.h"
#include "pack/pack_manager.h"
#include "render/compositor.h"
#include "script/lua_engine.h"
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* why) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
void Put32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (int i=0; i<4; ++i) bytes.push_back((value >> (8*i)) & 255);
}
const artc::Layer* Find(const artc::Compositor& c, const char* id) {
    for (const auto& layer : c.Layers()) if (layer.id == id) return &layer;
    return nullptr;
}
void Size(const artc::Compositor& c, const char* id, float expected, const char* why) {
    const auto* layer = Find(c, id);
    // rectangle.ttf's ink box is 600x700 units per 1000-unit em.
    Check(layer && layer->glyphs.size()==1 &&
          std::abs(layer->glyphs[0].w-std::ceil(expected*0.6f))<=1 &&
          std::abs(layer->glyphs[0].h-std::ceil(expected*0.7f))<=1, why);
}
// rectangle.ttf has equal em and hhea heights. Derive an independent
// runtime-only stb fixture to distinguish the two sizes without changing
// the original outlines, OS/2 baseline, or shared regression asset.
std::vector<uint8_t> TallMetricsFont(const std::vector<uint8_t>& original,
                                     int ascent = 1200, int descent = -300,
                                     bool invalid_os2 = false) {
    auto bytes = original;
    const auto u16 = [&](size_t at) -> uint16_t {
        return (uint16_t(bytes.at(at)) << 8) | bytes.at(at+1);
    };
    const auto u32 = [&](size_t at) -> uint32_t {
        return (uint32_t(u16(at)) << 16) | u16(at+2);
    };
    const auto put16 = [&](size_t at, int value) {
        bytes.at(at) = (uint16_t(value) >> 8) & 255;
        bytes.at(at+1) = uint16_t(value) & 255;
    };
    if (bytes.size() < 12) return {};
    const size_t count = u16(4);
    if (count > (bytes.size()-12)/16) return {};
    bool hhea_found = false, os2_found = false;
    for (size_t i=0; i<count; ++i) {
        const size_t record=12+i*16;
        const uint32_t tag=u32(record);
        if (tag != 0x68686561 && tag != 0x4f532f32) continue; // hhea, OS/2
        const size_t offset=u32(record+8), length=u32(record+12);
        const size_t minimum=tag==0x68686561 ? 36 : 72;
        if (length < minimum || offset > bytes.size() || length > bytes.size()-offset)
            return {};
        if (tag == 0x68686561) {
            put16(offset+4, ascent); put16(offset+6, descent);
            hhea_found = true;
        } else {
            if (invalid_os2) { put16(offset+68, 0); put16(offset+70, 0); }
            os2_found = true;
        }
    }
    // stb does not consume sfnt table checksums in this in-memory fixture.
    // Its em stays 1000, and no font is redistributed or written separately.
    return hhea_found && (!invalid_os2 || os2_found) ? bytes : std::vector<uint8_t>{};
}
int InkHeight(artc::Compositor& c, int width, int height) {
    glViewport(0,0,width,height);
    c.Draw();
    std::vector<uint8_t> pixels(static_cast<size_t>(width)*height*4);
    glReadPixels(0,0,width,height,GL_RGBA,GL_UNSIGNED_BYTE,pixels.data());
    int first=height, last=-1;
    for (int y=0; y<height; ++y)
        for (int x=0; x<width; ++x)
            if (pixels[(static_cast<size_t>(y)*width+x)*4]>128) {
                first=std::min(first,y); last=std::max(last,y);
            }
    Check(glGetError()==GL_NO_ERROR, "font rendering has no GLES error");
    return last<first ? 0 : last-first+1;
}
}

int main() {
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Check(eglInitialize(display,nullptr,nullptr), "initialize EGL");
    const EGLint attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config=nullptr; EGLint count=0;
    Check(eglChooseConfig(display,attrs,&config,1,&count) && count, "choose EGL config");
    const EGLint surface_attrs[]={EGL_WIDTH,512,EGL_HEIGHT,384,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,surface_attrs);
    const EGLint context_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,context_attrs);
    Check(eglMakeCurrent(display,surface,surface,context), "make EGL context current");
    if (failures) return 1;
    std::ifstream font(std::string(ARTC_TEST_DATA)+"/rectangle.ttf",std::ios::binary);
    Check(bool(font), "open synthetic font");
    std::vector<uint8_t> font_data{std::istreambuf_iterator<char>(font),{}};
    if (failures) return 1;
    const std::string name="font.ttf";
    const uint32_t index_size=4+16+name.size();
    std::vector<uint8_t> pack={'p','f','8'};
    Put32(pack,index_size); Put32(pack,1); Put32(pack,name.size());
    pack.insert(pack.end(),name.begin(),name.end());
    Put32(pack,0); Put32(pack,7+index_size); Put32(pack,font_data.size());
    pack.insert(pack.end(),font_data.begin(),font_data.end());
    const auto path=std::filesystem::temp_directory_path()/
        ("artc_font_style_"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pfs");
    { std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(pack.data()),pack.size()); }
    artc::PackManager packs;
    Check(packs.OpenChain(path.string(),{0}), "open synthetic font pack");
    {
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&packs);
        Check(c.LoadFont(name), "load synthetic font through compositor");
        artc::LuaEngine script;
        Check(script.Init(&packs,artc::Ini{},"android",256,192,&c), "initialize font style script");
        Check(script.DoString(R"(
            e:tag{'fontdefault',face='font.ttf',size=20,width=200}
            e:tag{'chgmsg',id='title'}
            e:tag{'font',size=60,width=850}
            e:tag{'print',data='A'}
            e:tag{'/chgmsg'}
            e:tag{'chgmsg',id='body'}
            e:tag{'print',data='A'}
        )", "title and default body"), "execute title and body tags");
        Size(c,"title",60,"explicit title size overrides game defaults");
        Size(c,"body",20,"new body uses defaults rather than the most recent title font");
        Check(script.DoString(R"(
            e:tag{'fontinit'}
            e:tag{'font',width=180}
            e:tag{'rp'}; e:tag{'print',data='A'}
        )", "reset current font"), "execute current font reset");
        Size(c,"body",20,"fontinit retains declared defaults when later font omits size");
        Size(c,"title",60,"resetting body does not erase another layer's explicit style");
        Check(script.DoString(R"(
            e:tag{'font',size=30}
            e:tag{'rp'}; e:tag{'print',data='A'}
            e:tag{'chgmsg',id='icon_name'}
            e:tag{'font',size=12,width=40,show='none'}
            e:tag{'print',data='A'}
            e:tag{'/chgmsg'}
            assert(e:var('s.current_message_layer')=='body')
            e:tag{'rp'}; e:tag{'print',data='A'}
        )", "nested explicit fonts"), "execute nested style tags");
        Size(c,"body",30,"nested font selection restores the body's explicit size");
        Size(c,"icon_name",12,"hidden font slot retains its own size without becoming the main font");
        Check(script.DoString(R"(
            e:tag{'fontdefault',face='font.ttf',size=18,width=200}
            e:tag{'chgmsg',id='new_name'}; e:tag{'print',data='A'}
            e:tag{'/chgmsg'}
            e:tag{'rp'}; e:tag{'print',data='A'}
        )", "change declared defaults"), "execute updated defaults");
        Size(c,"new_name",18,"unconfigured layer uses new defaults without inheriting another slot");
        Size(c,"body",30,"declaring defaults does not replace an existing explicit layer size");
        Check(script.DoString("e:tag{'fontinit'}; e:tag{'rp'}; e:tag{'print',data='A'}",
                              "reset without another font tag"), "reset current overrides before printing");
        Size(c,"body",18,"fontinit resets only current overrides and print still resolves defaults");
        c.Shutdown();
    }
    {
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&packs);
        Check(c.LoadFont(name), "load partial-default synthetic font");
        artc::LuaEngine script;
        Check(script.Init(&packs,artc::Ini{},"android",256,192,&c), "initialize partial defaults script");
        Check(script.DoString(R"(
            e:tag{'fontdefault',face='font.ttf'}
            e:tag{'font',size=24,width=850}
            e:tag{'chgmsg',id='body'}; e:tag{'print',data='A'}; e:tag{'/chgmsg'}
        )", "face-only defaults"), "execute face-only default and legacy visible font");
        Size(c,"body",24,"face-only defaults retain the legacy visible-slot size");
        Check(script.DoString(R"(
            e:tag{'fontdefault',color='FF0000'}
            e:tag{'chgmsg',id='color_only'}; e:tag{'print',data='A'}; e:tag{'/chgmsg'}
        )", "color-only defaults"), "execute color-only defaults");
        Size(c,"color_only",24,"color-only defaults retain an unspecified legacy size");
        c.SetProps("color_only",{{"left","30"},{"top","0"}});
        glViewport(0,0,256,192); c.Draw();
        uint8_t pixel[4]{};
        glReadPixels(33,192-1-8,1,1,GL_RGBA,GL_UNSIGNED_BYTE,pixel);
        Check(pixel[0]>200 && pixel[1]<10 && pixel[2]<10,
              "declared default color overrides the legacy slot in actual pixels");
        Check(script.DoString(R"(
            e:tag{'fontdefault',face='font.ttf',size=20,width=200}
            e:tag{'chgmsg',id='declared_size'}; e:tag{'print',data='A'}; e:tag{'/chgmsg'}
            e:tag{'chgmsg',id='explicit_size'}; e:tag{'font',size=32}
            e:tag{'print',data='A'}; e:tag{'/chgmsg'}
        )", "size precedence with legacy slot"), "execute declared and explicit size precedence");
        Size(c,"declared_size",20,"a declared default size overrides the previous visible-slot size");
        Size(c,"explicit_size",32,"a layer's explicit size overrides both declared and legacy sizes");
        c.Shutdown();
    }
    {
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&packs);
        Check(c.LoadFont(name), "load legacy synthetic font");
        artc::LuaEngine script;
        Check(script.Init(&packs,artc::Ini{},"android",256,192,&c), "initialize legacy text script");
        Check(script.DoString("e:tag{'chgmsg',id='body'}; e:tag{'print',data='A'}", "legacy size"),
              "execute text without any font defaults");
        Size(c,"body",40,"unconfigured legacy text keeps the original 40-unit default");
        c.Shutdown();
    }
    {
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&packs);
        Check(c.LoadFont(name), "load font for stage scaling");
        Check(c.SetText("1.body","A",40,0xffffff,200), "rasterize original stage text");
        Size(c,"1.body",40,"raster size stays in stage units");
        const int at_stage=InkHeight(c,256,192);
        const int at_surface=InkHeight(c,512,384);
        Check(at_stage>=27 && at_stage<=29 && std::abs(at_surface-2*at_stage)<=2,
              "doubling the surface scales the whole stage once without changing font size");
        c.SetProps("1",{{"xscale","150"},{"yscale","150"}});
        const auto* layer=Find(c,"1.body");
        const auto transform=c.EffectiveTransform(*layer);
        Check(std::abs(transform.a-1.5f)<0.001f && std::abs(transform.d-1.5f)<0.001f,
              "parent scale is applied once by the layer transform");
        const int scaled=InkHeight(c,512,384);
        Check(std::abs(scaled-3*at_stage)<=3,
              "parent and surface scaling compose once each in actual pixels");
        Size(c,"1.body",40,"surface and parent changes do not enlarge glyph raster dimensions");
        c.Shutdown();
    }
    {
        const auto tall_font=TallMetricsFont(font_data);
        Check(!tall_font.empty(), "derive independent font with hhea height 1500 and em 1000");
        if (!tall_font.empty()) {
            std::vector<uint8_t> tall_pack={'p','f','8'};
            Put32(tall_pack,index_size); Put32(tall_pack,1); Put32(tall_pack,name.size());
            tall_pack.insert(tall_pack.end(),name.begin(),name.end());
            Put32(tall_pack,0); Put32(tall_pack,7+index_size); Put32(tall_pack,tall_font.size());
            tall_pack.insert(tall_pack.end(),tall_font.begin(),tall_font.end());
            const auto tall_path=std::filesystem::path(path.string()+".height.pfs");
            { std::ofstream out(tall_path,std::ios::binary);
              out.write(reinterpret_cast<const char*>(tall_pack.data()),tall_pack.size()); }
            artc::PackManager tall_packs;
            Check(tall_packs.OpenChain(tall_path.string(),{0}), "open tall-metrics font pack");
            artc::Compositor c; c.Init(256,192); c.SetPackManager(&tall_packs);
            Check(c.LoadFont(name), "load tall-metrics font through compositor");
            artc::LuaEngine script;
            Check(script.Init(&tall_packs,artc::Ini{},"android",256,192,&c),
                  "initialize tall-metrics font script");
            Check(script.DoString(R"(
                e:tag{'chgmsg',id='height.body'}
                e:tag{'font',size=30,width=60,kerning=0}
                e:tag{'print',data='AAAA'}
            )", "pixel-height body"), "execute tall-metrics font and print tags");
            const auto* body=Find(c,"height.body");
            // 600x700 ink at scale 30/1500, advance 700*30/1500 = 14.
            Check(body && body->glyphs.size()==4 &&
                  std::abs(body->glyphs[0].w-12)<=1 &&
                  std::abs(body->glyphs[0].h-14)<=1,
                  "body size uses vertical pixel height instead of em height");
            if (body && body->glyphs.size()==4) {
                Check(std::abs(body->glyphs[1].x-body->glyphs[0].x-14)<0.01f &&
                      std::abs(body->glyphs[3].y-body->glyphs[0].y)<0.01f,
                      "advance uses the same height scale and four glyphs fit width 60");
                // OS/2 ascender 800 and ink top 700 yield (800-700)*.02=2.
                Check(std::abs(body->glyphs[0].y-2)<0.01f,
                      "existing OS/2 baseline remains independent of the sizing metric");
            }
            Check(script.DoString(R"(
                e:tag{'font',kerning=2}; e:tag{'rp'}; e:tag{'print',data='AAAA'}
            )", "height-scale tracking"), "execute tracking and wrap tags");
            body=Find(c,"height.body");
            if (body && body->glyphs.size()==4) {
                Check(std::abs(body->glyphs[1].x-body->glyphs[0].x-16)<0.01f &&
                      std::abs(body->glyphs[3].x-body->glyphs[0].x)<0.01f &&
                      std::abs(body->glyphs[3].y-body->glyphs[0].y-30)<0.01f,
                      "tracking is applied after scaling and wraps only the fourth glyph");
            } else Check(false, "tracking text retains four positioned glyphs");
            Check(c.SetText("height.ruby","A",30,0xffffff,100,
                            {{"rubysize","15"}}, {{0,1,"A"}}),
                  "rasterize ruby with explicit pixel height");
            const auto* ruby=Find(c,"height.ruby");
            Check(ruby && ruby->glyphs.size()==2 &&
                  std::abs(ruby->glyphs[0].w-12)<=1 &&
                  std::abs(ruby->glyphs[0].h-14)<=1 &&
                  std::abs(ruby->glyphs[1].w-6)<=1 &&
                  std::abs(ruby->glyphs[1].h-7)<=1,
                  "ruby and body share vertical pixel-height sizing");
            // At the same CELL raster size, effect-cell padding compensates
            // the sample's negative tracking instead of shrinking the ink.
            Check(c.SetText("spacing.body","AAAA",30,0xffffff,55,
                  {{"outline","1"},{"shadow","1"},{"kerning","-3"}}),
                  "layout effect cell with negative body tracking");
            const auto* spaced=Find(c,"spacing.body");
            Check(spaced && spaced->glyphs.size()==4 &&
                  std::abs(spaced->glyphs[1].x-spaced->glyphs[0].x-14)<0.01f &&
                  spaced->glyphs[3].y>spaced->glyphs[0].y &&
                  std::abs(spaced->glyphs[0].w-14)<=1 &&
                  std::abs(spaced->glyphs[0].h-16)<=1,
                  "outline and shadow take layout width without changing fill size");
            // This sample-style negative spacing exposes a shortened OS/2
            // line box even though its CELL-sized ink is already correct.
            const std::map<std::string, std::string> line_style = {
                {"outline","1"},{"shadow","1"},{"kerning","-3"},
                {"spacetop","0"},{"rubysize","14"},
                {"spacemiddle","-12"},{"spacebottom","-10"}};
            Check(c.SetText("lineheight.explicit","A\nA",30,0xffffff,100,line_style),
                  "layout explicit newline with tall metrics and negative row spacing");
            const auto* lines=Find(c,"lineheight.explicit");
            Check(lines && lines->glyphs.size()==2 &&
                  std::abs(lines->glyphs[1].y-lines->glyphs[0].y-25)<0.01f &&
                  lines->glyphs[0].y+lines->glyphs[0].h<=lines->glyphs[1].y &&
                  std::abs(lines->glyphs[0].y-4)<0.01f &&
                  std::abs(lines->glyphs[0].w-14)<=1 &&
                  std::abs(lines->glyphs[0].h-16)<=1,
                  "requested cell height prevents line overlap while retaining first-line ink");
            Check(c.SetText("lineheight.wrap","AAAA",30,0xffffff,55,line_style),
                  "wrap outlined text with tall metrics and negative row spacing");
            const auto* wrapped=Find(c,"lineheight.wrap");
            Check(wrapped && wrapped->glyphs.size()==4 &&
                  std::abs(wrapped->glyphs[1].x-wrapped->glyphs[0].x-14)<0.01f &&
                  std::abs(wrapped->glyphs[3].y-wrapped->glyphs[0].y-25)<0.01f &&
                  wrapped->glyphs[0].y+wrapped->glyphs[0].h<=wrapped->glyphs[3].y,
                  "automatic wrapping uses the same non-overlapping row height");
            Check(c.SetText("spacing.ruby","AA",30,0xffffff,100,
                  {{"rubysize","15"},{"outline","1"},{"shadow","1"},
                   {"kerning","-3"},{"rubykerning","2"}},{{0,2,"AA"}}),
                  "layout ruby with independent tracking");
            const auto* reading=Find(c,"spacing.ruby");
            Check(reading && reading->glyphs.size()==4 &&
                  std::abs(reading->glyphs[1].x-reading->glyphs[0].x-14)<0.01f &&
                  std::abs(reading->glyphs[3].x-reading->glyphs[2].x-12)<0.01f,
                  "ruby uses its own tracking plus the effect cell width");
            c.Shutdown();
            std::filesystem::remove(tall_path);
        }
    }
    // Empty/reversed vertical metrics are accepted by stb's InitFont, so
    // SetText must resolve a safe height before computing raster geometry.
    for (bool invalid_os2 : {false, true}) {
        const auto metric_font=TallMetricsFont(font_data, 0, 0, invalid_os2);
        Check(!metric_font.empty(), "derive zero-hhea font with controlled OS/2 metrics");
        if (metric_font.empty()) continue;
        std::vector<uint8_t> metric_pack={'p','f','8'};
        Put32(metric_pack,index_size); Put32(metric_pack,1); Put32(metric_pack,name.size());
        metric_pack.insert(metric_pack.end(),name.begin(),name.end());
        Put32(metric_pack,0); Put32(metric_pack,7+index_size); Put32(metric_pack,metric_font.size());
        metric_pack.insert(metric_pack.end(),metric_font.begin(),metric_font.end());
        const auto metric_path=std::filesystem::path(path.string()+
            (invalid_os2 ? ".invalid-height.pfs" : ".fallback-height.pfs"));
        { std::ofstream out(metric_path,std::ios::binary);
          out.write(reinterpret_cast<const char*>(metric_pack.data()),metric_pack.size()); }
        artc::PackManager metric_packs;
        Check(metric_packs.OpenChain(metric_path.string(),{0}), "open zero-hhea font pack");
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&metric_packs);
        Check(c.LoadFont(name), "load zero-hhea font through compositor");
        const bool rendered=c.SetText("safe.height","A",30,0xffffff,100,
                                      {{"rubysize","15"}}, {{0,1,"A"}});
        if (invalid_os2) {
            Check(!rendered && !Find(c,"safe.height"),
                  "font with no positive vertical metric fails before publishing glyphs");
        } else {
            const auto* layer=Find(c,"safe.height");
            Check(rendered && layer && layer->glyphs.size()==2,
                  "zero hhea falls back to the positive OS/2 height for body and ruby");
            if (layer && layer->glyphs.size()==2) {
                Check(std::abs(layer->glyphs[0].w-18)<=1 &&
                      std::abs(layer->glyphs[0].h-21)<=1 &&
                      std::abs(layer->glyphs[1].w-9)<=1 &&
                      std::abs(layer->glyphs[1].h-11)<=1,
                      "OS/2 1000-unit height scales body 30 and ruby 15 consistently");
                for (const auto& glyph : layer->glyphs)
                    Check(std::isfinite(glyph.x) && std::isfinite(glyph.y) &&
                          std::isfinite(glyph.w) && std::isfinite(glyph.h),
                          "fallback glyph geometry remains finite");
            }
        }
        c.Shutdown();
        std::filesystem::remove(metric_path);
    }
    // Watermark eviction must be a recoverable one-shot event. Distinct fill
    // colors force real atlas misses and cross the six-page limit; HEADLESS
    // mocks cannot cover this path.
    {
        artc::Compositor c; c.Init(256,192); c.SetPackManager(&packs);
        Check(c.LoadFont(name), "load font for atlas eviction");
        int first_failure=-1;
        for(int i=0;i<400;++i) {
            if(!c.SetText("flood."+std::to_string(i),"A",256,
                          0x100000+static_cast<uint32_t>(i),300)) {
                first_failure=i;
                break;
            }
        }
        Check(first_failure<0, "SetText retries successfully after atlas eviction");
        const bool recovered=c.SetText("after.eviction","A",32,0xffffff,100);
        const auto* after=Find(c,"after.eviction");
        Check(recovered && after && after->glyphs.size()==1,
              "unrelated text still renders after atlas eviction");
        c.Shutdown();
    }
    std::filesystem::remove(path);
    eglMakeCurrent(display,EGL_NO_SURFACE,EGL_NO_SURFACE,EGL_NO_CONTEXT);
    eglDestroyContext(display,context); eglDestroySurface(display,surface); eglTerminate(display);
    if (!failures) std::cout << "font style regressions passed\n";
    return failures ? 1 : 0;
}