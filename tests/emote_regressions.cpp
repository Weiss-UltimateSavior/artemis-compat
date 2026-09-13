#include "pack/psb.h"
#include "render/emote_model.h"
#include "render/emote_scene.h"
#include "render/emote_player.h"
#include "render/compositor.h"
#include "emote_scene_fixture.h"
#include <cmath>
#include <memory>
#include <zlib.h>
#include <cstdlib>
#include <fstream>
#include <iostream>
#if defined(ARTC_HAS_GLES)
#include <EGL/egl.h>
#endif

static void Check(bool ok,const char* why) {
    if(!ok){std::cerr<<why<<'\n';std::exit(1);}
}
static std::vector<uint8_t> Fixture() {
    std::vector<uint8_t> b(44);b[0]='P';b[1]='S';b[2]='B';b[4]=3;
    auto field=[&](size_t at){for(int i=0;i<4;++i)b[at+i]=uint8_t(b.size()>>(8*i));};
    auto array=[&](const std::vector<uint8_t>& a){b.push_back(13);b.push_back(uint8_t(a.size()));b.push_back(13);b.insert(b.end(),a.begin(),a.end());};
    field(12);array({0});std::vector<uint8_t> parent(99);parent[98]=97;array(parent);array({98}); // trie: "a"
    field(16);array({0});field(20);b.insert(b.end(),{'h','i',0});
    field(24);array({0});field(28);array({4});field(32);b.insert(b.end(),{255,0,0,255});
    field(36);b.push_back(33);array({0});array({0}); // {a: ["hi", resource0, -2, true]}
    b.push_back(32);array({0,2,4,6});b.insert(b.end(),{21,0,25,0,5,254,3});return b;
}
static artc::PsbValue N(double n) {artc::PsbValue v;v.type=artc::PsbValue::Number;v.number=n;return v;}
static artc::PsbValue S(const std::string& s) {artc::PsbValue v;v.type=artc::PsbValue::String;v.string=s;return v;}
static artc::PsbValue A(std::initializer_list<artc::PsbValue> a) {artc::PsbValue v;v.type=artc::PsbValue::Array;v.array=a;return v;}
static artc::PsbValue O(std::initializer_list<std::pair<const std::string,artc::PsbValue>> o) {artc::PsbValue v;v.type=artc::PsbValue::Object;v.object=o;return v;}
static void ModelTests() {
    auto key=[](double t,double v,double e=0){return O({{"time",N(t)},{"type",N(2)},{"content",O({{"value",N(v)},{"easing",N(e)}})}});};
    auto track=[&](const char* label){return O({{"label",S(label)},{"frameList",A({key(0,0),key(10,10,1),O({{"time",N(20)},{"type",N(0)}})})}});};
    artc::PsbDocument doc;doc.root=O({{"spec",S("krkr")},{"object",O({{"body",O({{"motion",O({{"idle",O({})}})}})}})},
        {"metadata",O({{"base",O({{"chara",S("body")},{"motion",S("idle")}})},
        {"variableList",A({O({{"label",S("head")}}),O({{"label",S("eye")}})})},
        {"instantVariableList",A({S("eye")})},
        {"timelineControl",A({O({{"label",S("blink")},{"lastTime",N(-1)},{"loopBegin",N(0)},{"loopEnd",N(20)},
        {"variableList",A({track("head"),track("eye")})}})})}})}});
    artc::EmoteModel model;std::string error;Check(model.Load(doc,error),error.c_str());
    std::map<std::string,double> values;
    Check(model.Sample("blink",5,values) && std::abs(values.at("head")-2.5)<1e-6 && values.at("eye")==0,"continuous easing and discrete expression tracks");
    Check(model.Sample("blink",15,values) && values.at("head")==10 && values.at("eye")==10,"terminal marker retains previous value");
    Check(model.Sample("blink",45,values) && std::abs(values.at("head")-2.5)<1e-6,"timeline seek wraps multiple loops");
    Check(!model.Sample("missing",0,values) && values.count("head"),"unknown timeline preserves output");
    doc.root.object["metadata"].object["base"].object["motion"]=S("missing");
    Check(!model.Load(doc,error) && model.Timelines().size()==1,"invalid model preserves prior timelines");
    doc.root.object["metadata"].object["base"].object["motion"]=S("idle");
    artc::PsbValue resource;resource.type=artc::PsbValue::Resource;
    doc.root.object["source"]=O({{"face",O({{"icon",O({{"smile",O({{"width",N(1)},{"height",N(1)},
        {"originX",N(3.5)},{"originY",N(-2)},{"type",S("RGBA8")},{"pixel",resource},{"pal",resource}})}})}})}});
    doc.bytes={10,20,30,128};doc.resources={{0,4}};
    Check(model.Load(doc,error),error.c_str());artc::EmoteImage image;
    Check(model.Image("face","smile",image,error) && image.rgba==std::vector<uint8_t>({30,20,10,128}) && image.origin_x==3.5 && image.origin_y==-2,
        "desktop color order, independent sprite origins and unused palette reference");
    auto& icon=doc.root.object["source"].object["face"].object["icon"].object["smile"];
    icon.object["type"]=S("CI8");icon.object["pal"].resource=1;
    doc.bytes={0,10,20,30,128};doc.resources={{0,1},{1,4}};
    Check(model.Load(doc,error) && model.Image("face","smile",image,error) && image.rgba[0]==30,"CI8 palette expansion");
    doc.bytes[0]=1;Check(model.Load(doc,error),error.c_str());
    Check(!model.Image("face","smile",image,error) && image.rgba[0]==30,"invalid texture retains prior pixels");
}
// The playback contract: frame clock, queue semantics, fades, hold-end,
// skip/pass, difference-vs-lerp variable mixing (observed through the
// parameterized face icon) and Load atomicity.
static void PlayerTests() {
    std::string error;
    auto document=emote_fixture::PlayerDocument();
    // The encoder round-trips through the real decoder before anything trusts it.
    {artc::PsbDocument decoded;const auto bytes=emote_fixture::EncodePsb(document);
     Check(artc::DecodePsb(bytes,decoded,error),error.c_str());
     Check(decoded.root.At("metadata").At("timelineControl").array.size()==3,"test PSB writer round-trips timelineControl");}
    auto model=std::make_shared<artc::EmoteModel>();
    Check(model->Load(std::move(document),error),error.c_str());

    artc::EmotePlayer player;
    Check(!player.Active(),"player starts inactive");
    Check(!player.Load(nullptr,error) && error.find("missing")!=std::string::npos,"null model rejected");
    Check(player.Load(model,error),error.c_str());
    Check(player.Active(),"loaded player is active");
    Check(player.CountMainTimelines()==2 && player.MainTimelineLabelAt(0)=="loop" &&
          player.MainTimelineLabelAt(1)=="once","main timeline enumeration follows the model map");
    Check(player.CountDiffTimelines()==1 && player.DiffTimelineLabelAt(0)=="delta","difference timeline enumeration");
    Check(player.CountVariables()==1 && player.VariableLabelAt(0)=="expression","variable enumeration");
    Check(player.TimelineTotalFrames("once")==60 && player.TimelineTotalFrames("loop")==20 &&
          player.TimelineTotalFrames("missing")<0,"timeline lengths");
    Check(player.IsLoopTimeline("loop") && !player.IsLoopTimeline("once"),"loop detection");
    Check(!player.PlayTimeline("missing",0,error) && error.find("unknown")!=std::string::npos,"unknown timeline rejected");
    Check(player.PlayTimeline("once",0,error) && player.IsTimelinePlaying("once"),"play starts the timeline");
    Check(player.PlayTimeline("delta",artc::EmotePlayer::kTimelineSequential,error) &&
          !player.IsTimelinePlaying("delta") && player.CountPlayingTimelines()==1,"sequential queues behind live playback");
    player.Progress(1000);  // 60 frames: "once" holds its final pose, queue starts
    Check(!player.IsTimelinePlaying("once") && player.IsTimelinePlaying("delta") &&
          player.PlayingTimelineLabelAt(1)=="delta" &&
          player.PlayingTimelineFlagsAt(1)==artc::EmotePlayer::kTimelineSequential,"a freed slot starts the queued timeline");

    // transform round-trips
    player.SetCoord(5,6,0,0);double x=0,y=0;player.GetCoord(&x,&y);
    Check(x==5 && y==6,"coordinate round-trip");
    player.SetScale(2,3,0,0);player.GetScale(&x,&y);
    Check(x==2 && y==3,"scale round-trip");
    player.SetRot(90,0,0);Check(player.GetRot()==90,"rotation round-trip");
    player.SetColor(0x80ABCDEFu,0,0);Check(player.GetColor()==0x80ABCDEFu,"color round-trip");
    player.SetMirror(true);Check(player.IsMirrored(),"mirror flag");
    player.Hide();Check(player.IsHidden(),"hide flag");player.Show();

    // render + frame clock through the compositor (host SetPixels build)
    artc::Compositor compositor;
    Check(player.Render(compositor,"p",error),error.c_str());
    Check(compositor.GetLayerInfo("p").found && compositor.GetLayerInfo("p.000001").found,
          "render installs the container and scene layers");
    player.Progress(5000.0/3.0);  // +100 frames on top of the 60 already played
    Check(player.Render(compositor,"p",error),error.c_str());
    Check(std::abs(compositor.GetLayerInfo("p.000001").left-14.0)<1e-3,
          "progress advances the base motion at 60 fps");  // 160 frames wraps to 6 → 8+10*0.6
    artc::EmotePlayer ticking;
    Check(ticking.Load(model,error),error.c_str());
    Check(ticking.Update(0,&compositor,"q") && compositor.GetLayerInfo("q").found,"update renders");
    Check(ticking.Update(500,&compositor,"q"),error.c_str());  // dt 500 ms → 30 frames → frame 8
    Check(std::abs(compositor.GetLayerInfo("q.000001").left-16.0)<1e-3,"update ticks wall-clock deltas");

    // ComposeVariables is private — observe mixing through the parameterized
    // face icon: body picture is 4px wide, the face 2px ("face") or 4px ("wide").
    auto picture_sum=[&](const char* id) {
        int sum=0;
        for(const auto& l:compositor.Layers())
            if(l.id.rfind(id,0)==0 && l.id.size()>=7 && l.id.compare(l.id.size()-7,7,".000000")==0)
                sum+=int(compositor.GetLayerInfo(l.id).width);
        return sum;
    };
    artc::EmotePlayer mixer;
    Check(mixer.Load(model,error),error.c_str());
    Check(mixer.SetVariable("expression",6,0,0,error),error.c_str());
    Check(mixer.FadeInTimeline("loop",200,0,error),error.c_str());
    mixer.Progress(8000.0/60.0);  // timeline position 8, blend 2/3
    Check(mixer.Render(compositor,"r",error) && picture_sum("r")==6,
          "a main timeline lerps the base toward the sampled value");  // 6+(8-6)*2/3≈7.3 <10 → face
    mixer.StopTimeline("loop");
    Check(mixer.SetVariable("expression",6,0,0,error) && mixer.FadeInTimeline("delta",200,0,error),error.c_str());
    mixer.Progress(8000.0/60.0);
    Check(mixer.Render(compositor,"r",error) && picture_sum("r")==8,
          "a difference timeline adds on top of the base");  // 6+8*2/3≈11.3 → clamped 10 → wide

    // transitions and ease weights
    artc::EmotePlayer vars;
    Check(vars.Load(model,error),error.c_str());
    Check(!vars.IsAnimating(),"fresh player is still");
    Check(vars.SetVariable("expression",10,1000,1,error),error.c_str());
    vars.Progress(500);
    bool found=false;
    Check(vars.GetVariable("expression",&found)==2.5 && found,"ease >= 0 weighs ease+1");
    Check(vars.SetVariable("expression",10,1000,-1,error),error.c_str());
    vars.Progress(500);
    Check(std::abs(vars.GetVariable("expression",&found)-(2.5+7.5*std::sqrt(0.5)))<1e-9 && found,
          "ease < 0 weighs 1/(1-ease)");
    Check(vars.IsAnimating(),"a running transition keeps the player animating");
    vars.Pass();
    Check(vars.GetVariable("expression",&found)==10 && found && !vars.IsAnimating(),
          "pass completes transitions without touching timelines");
    Check(!vars.SetVariable("missing",1,0,0,error) && error.find("unknown")!=std::string::npos,
          "unknown variable rejected");
    Check(vars.GetVariable("missing",&found)==0 && !found,"unknown variable read reports missing");

    // queue semantics: restart-in-place, stop releasing the queue
    artc::EmotePlayer queue;
    Check(queue.Load(model,error),error.c_str());
    Check(queue.PlayTimeline("loop",0,error) && queue.PlayTimeline("once",1,error),error.c_str());
    Check(queue.PlayTimeline("loop",artc::EmotePlayer::kTimelineParallel,error) &&
          queue.PlayingTimelineLabelAt(0)=="loop" &&
          queue.PlayingTimelineLabelAt(1)=="once" && queue.PlayingTimelineFlagsAt(0)==1,
          "replaying a label restarts it in place with the new flags");
    queue.StopTimeline("once");
    Check(queue.CountPlayingTimelines()==1 && queue.PlayingTimelineLabelAt(0)=="loop","stop removes the live timeline");
    Check(queue.PlayTimeline("delta",2,error) && !queue.IsTimelinePlaying("delta"),error.c_str());
    queue.StopTimeline("loop");  // frees the slot → queued delta starts
    Check(queue.IsTimelinePlaying("delta"),"stopping the live timeline releases the sequential queue");
    queue.PlayTimeline("delta",2,error);  // busy → queued copy
    queue.StopTimeline("delta");
    Check(queue.CountPlayingTimelines()==0 && !queue.IsTimelinePlaying("delta"),"stop removes a queued copy too");

    // fades and manual blend
    Check(queue.FadeInTimeline("loop",200,0,error),error.c_str());
    Check(queue.TimelineBlendRatio("loop",&found)==0 && found,"fade-in starts silent");
    queue.Progress(100);
    Check(std::abs(queue.TimelineBlendRatio("loop",&found)-0.5)<1e-9 && found,"fade-in interpolates");
    Check(queue.FadeOutTimeline("loop",200,error),error.c_str());
    queue.Progress(200);  // blend reaches 0 → auto-stop
    Check(!queue.IsTimelinePlaying("loop") && queue.TimelineBlendRatio("loop",&found)==0 && !found,
          "fade-out removes the timeline at zero blend");
    Check(!queue.FadeOutTimeline("loop",100,error) && error.find("not playing")!=std::string::npos,
          "fading an idle timeline fails");
    Check(queue.FadeInTimeline("loop",100,0,error),error.c_str());
    queue.Progress(100);  // fade completes → blend 1
    Check(queue.FadeOutTimeline("loop",1000,error),error.c_str());
    queue.Progress(100);  // blend 1 → 0.9
    Check(queue.SetTimelineBlendRatio("loop",0.5,error),error.c_str());
    queue.Progress(2000);
    Check(queue.TimelineBlendRatio("loop",&found)==0.5 && found && queue.IsTimelinePlaying("loop"),
          "a direct blend set cancels the fade and its auto-stop");
    Check(queue.SetTimelineHoldEnd("loop",false,error),error.c_str());
    queue.Progress(1000);  // 60 frames ≥ loop end 20 → parks
    Check(!queue.IsTimelinePlaying("loop"),"hold-end parks a looping timeline at its loop end");

    // skip jumps finite timelines to their end and frees the queue
    artc::EmotePlayer skipper;
    Check(skipper.Load(model,error),error.c_str());
    Check(skipper.PlayTimeline("once",1,error) && skipper.PlayTimeline("delta",2,error),error.c_str());
    Check(skipper.SetVariable("expression",10,5000,0,error),error.c_str());
    skipper.Skip();
    Check(skipper.GetVariable("expression",&found)==10 && found,"skip completes transitions");
    Check(skipper.IsTimelinePlaying("delta") && !skipper.IsTimelinePlaying("once"),
          "skip jumps a finite timeline to its end and frees the queue");
    skipper.StopAllTimelines();
    Check(skipper.CountPlayingTimelines()==0,"stop-all clears the player");

    // a failed Load keeps the previous player intact
    auto broken=emote_fixture::PlayerDocument();
    broken.root.object["metadata"].object["base"].object["motion"]=S("missing");
    auto bad=std::make_shared<artc::EmoteModel>();
    Check(!bad->Load(std::move(broken),error),"broken model rejected");
    Check(!player.Load(bad,error) && player.Active() && player.IsTimelinePlaying("delta"),
          "a failed reload keeps the previous player intact");
}
int main(int argc,char** argv) {
#if defined(ARTC_HAS_GLES)
    // SetPixels uploads through real GL; OHOS builds the compositor with GLES
    // unconditionally, so give the binary a pbuffer context (as the desktop
    // ANGLE compositor suite does) before anything renders.
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if(display==EGL_NO_DISPLAY || !eglInitialize(display,nullptr,nullptr)) {
        std::cerr<<"initialize EGL: error 0x"<<std::hex<<eglGetError()<<std::endl;
        std::exit(1);
    }
    EGLint cfg_attrs[]={EGL_SURFACE_TYPE,EGL_PBUFFER_BIT,EGL_RENDERABLE_TYPE,EGL_OPENGL_ES2_BIT,
                        EGL_RED_SIZE,8,EGL_GREEN_SIZE,8,EGL_BLUE_SIZE,8,EGL_ALPHA_SIZE,8,EGL_NONE};
    EGLConfig config;EGLint count=0;
    Check(eglChooseConfig(display,cfg_attrs,&config,1,&count)&&count,"choose EGL config");
    EGLint pb_attrs[]={EGL_WIDTH,32,EGL_HEIGHT,32,EGL_NONE};
    EGLSurface surface=eglCreatePbufferSurface(display,config,pb_attrs);
    EGLint ctx_attrs[]={EGL_CONTEXT_CLIENT_VERSION,2,EGL_NONE};
    EGLContext context=eglCreateContext(display,config,EGL_NO_CONTEXT,ctx_attrs);
    Check(eglMakeCurrent(display,surface,surface,context),"make EGL context current");
#endif
    ModelTests();
    PlayerTests();
    {
        auto data=emote_fixture::Scene();auto model=std::make_shared<artc::EmoteModel>();std::string error;
        Check(model->Load(data,error),error.c_str());artc::EmoteScene scene;Check(scene.Load(model,error),error.c_str());
        std::vector<artc::EmoteSceneLayer> layers;
        Check(scene.Evaluate(5,{{"expression",10}},layers,error) && layers.front().x==13 && layers.back().icon=="wide",
            "E-mote child motion, variable range binding and continuous parent frame");
        Check(scene.Evaluate(100,{},layers,error) && layers.front().x==9 && layers.back().icon=="face","scene time wraps the base motion loop");
        const auto size=layers.size();
        Check(!scene.Evaluate(-1,{},layers,error) && layers.size()==size,"invalid scene time preserves output");
        data.root.object["object"].object["actor"].object["motion"].object["idle"].object["layer"].array[0]
            .object["frameList"].array[1].object["content"].object["mesh"]=emote_fixture::A({});
        auto bad=std::make_shared<artc::EmoteModel>();Check(bad->Load(data,error),error.c_str());
        Check(!scene.Load(bad,error) && error.find("mesh")!=std::string::npos && scene.Evaluate(5,{},layers,error),
            "unsupported later deformation fails before replacing a working scene");
    }
    const auto fixture=Fixture();artc::PsbDocument doc;std::string error;
    Check(artc::DecodePsb(fixture,doc,error),error.c_str());
    const auto& a=doc.root.At("a").array;
    Check(a.size()==4 && a[0].string=="hi" && a[2].number==-2 && a[3].number==1,"PSB dictionaries, arrays, signed numbers and strings");
    std::vector<uint8_t> pixels;
    Check(doc.ReadResource(a[1],pixels) && pixels==std::vector<uint8_t>({255,0,0,255}),"PSB resource ranges");
    for(size_t size=0;size<fixture.size();++size) {
        const std::vector<uint8_t> cut(fixture.begin(),fixture.begin()+size);
        Check(!artc::DecodePsb(cut,doc,error) && doc.root.At("a").array.size()==4,"truncated PSB leaves previous document intact");
    }
    auto broken=fixture;broken[6]=1;
    Check(!artc::DecodePsb(broken,doc,error) && error.find("encrypted")!=std::string::npos,"encrypted PSB reports unsupported");
    // The root's sole value offset loops back to the root dictionary.
    broken=fixture;broken[36]=255;broken[37]=255;broken[38]=255;broken[39]=255;
    Check(!artc::DecodePsb(broken,doc,error),"PSB rejects overflowing offsets");
    std::vector<uint8_t> mdf={'m','d','f',0};for(int i=0;i<4;++i)mdf.push_back(uint8_t(fixture.size()>>(8*i)));
    uLongf len=compressBound(fixture.size());mdf.resize(8+len);
    Check(compress2(mdf.data()+8,&len,fixture.data(),fixture.size(),6)==Z_OK,"MDF compress fixture");mdf.resize(8+len);
    Check(artc::DecodePsb(mdf,doc,error),error.c_str());mdf.back()^=1;
    Check(!artc::DecodePsb(mdf,doc,error),"MDF checksum failure");
    Check(artc::DecodePsbRl({128,1,2,3,4,0,5,6,7,8},4,4,pixels) && pixels.size()==16 && pixels[12]==5,"RL repeat and literal complete pixels");
    Check(artc::DecodePsbRl({128,7,0,9},4,1,pixels) && pixels==std::vector<uint8_t>({7,7,7,9}),"RL palette indices");
    Check(!artc::DecodePsbRl({129,7},3,1,pixels) && pixels.back()==9,"RL rejects oversized packet transactionally");
    Check(!artc::DecodePsbRl({128,7},4,1,pixels),"RL rejects undersized stream");
    Check(!artc::DecodePsbRl({128,7,0,9},3,1,pixels),"RL rejects trailing packet");
    for(int i=1;i<argc;++i) {
        std::ifstream f(argv[i],std::ios::binary);std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),{});
        Check(artc::DecodePsb(bytes,doc,error),error.c_str());
        std::cout<<"PSB v"<<doc.version<<" spec="<<doc.root.At("spec").string
                 <<" resources="<<doc.resources.size()<<" timelines="<<doc.root.At("metadata").At("timelineControl").array.size()<<'\n';
        artc::EmoteModel model;Check(model.Load(std::move(doc),error),error.c_str());size_t images=0;
        for(const auto& source:model.Document().root.At("source").object)
            for(const auto& icon:source.second.At("icon").object) {
                artc::EmoteImage image;Check(model.Image(source.first,icon.first,image,error),error.c_str());++images;
            }
        for(const auto& t:model.Timelines()) {
            std::map<std::string,double> values;Check(model.Sample(t.first,100.25,values),"sample real model track");
            for(const auto& v:values)Check(std::isfinite(v.second),"real timeline produces finite variables");
        }
        std::cout<<"decoded_images="<<images<<" variables="<<model.Variables().size()<<'\n';
        artc::EmoteScene scene;
        const bool supported=scene.Load(std::make_shared<artc::EmoteModel>(std::move(model)),error);
        std::cout<<"scene_renderer="<<(supported?"supported":error)<<'\n';
    }
}
