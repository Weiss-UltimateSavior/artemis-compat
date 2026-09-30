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
    // win/common shared-texture atlas: each icon crops left/top/width/height.
    artc::PsbDocument atlas_doc;atlas_doc.root=O({{"spec",S("common")},
        {"object",O({{"body",O({{"motion",O({{"idle",O({})}})}})}})},
        {"metadata",O({{"base",O({{"chara",S("body")},{"motion",S("idle")}})}})},
        {"source",O({{"body",O({{"texture",O({{"width",N(4)},{"height",N(2)},
            {"type",S("RGBA8")},{"pixel",resource}})},
            {"icon",O({{"art",O({{"left",N(2)},{"top",N(1)},{"width",N(2)},{"height",N(1)},
                {"originX",N(1)},{"originY",N(0.5)}})}})}})}})}});
    atlas_doc.bytes={10,20,30,40, 50,60,70,80, 90,100,110,120, 130,140,150,160,
                     170,180,190,200, 210,220,230,240, 250,1,2,3, 4,5,6,7};
    atlas_doc.resources={{0,32}};
    artc::EmoteModel atlas_model;
    Check(atlas_model.Load(atlas_doc,error),error.c_str());
    Check(atlas_model.Image("body","art",image,error) && image.width==2 && image.height==1 &&
          image.origin_x==1 && image.origin_y==0.5 &&
          image.rgba==std::vector<uint8_t>({250,1,2,3,4,5,6,7}),
          "shared texture atlas crop");
    // E8: the remaining desktop atlas formats decode to RGBA8.
    auto decode_pixels=[&](int w,int h,const char* type,std::vector<uint8_t> bytes,
                           std::vector<uint8_t>& rgba,bool mips=false)->bool {
        artc::PsbDocument doc;
        artc::PsbValue pixel_ref;pixel_ref.type=artc::PsbValue::Resource;
        artc::PsbValue icon=O({{"width",N(w)},{"height",N(h)},
            {"type",S(type)},{"pixel",pixel_ref}});
        if(mips)icon.object["mipMap"]=N(1);
        doc.root=O({{"spec",S("common")},
            {"object",O({{"body",O({{"motion",O({{"idle",O({})}})}})}})},
            {"metadata",O({{"base",O({{"chara",S("body")},{"motion",S("idle")}})}})},
            {"source",O({{"s",O({{"icon",O({{"i",icon}})}})}})}});
        doc.resources={{0,bytes.size()}};
        doc.bytes=std::move(bytes);
        artc::EmoteModel m;std::string error;artc::EmoteImage image;
        if(!m.Load(doc,error) || !m.Image("s","i",image,error))return false;
        rgba=image.rgba;return true;
    };
    {
        std::vector<uint8_t> rgba;
        // DXT1: c0 red > c1 blue, all indices 0 -> red.
        Check(decode_pixels(4,4,"DXT1",{0x00,0xF8,0x1F,0x00,0,0,0,0},rgba) &&
              rgba.size()==64 && rgba[0]==255 && rgba[1]==0 && rgba[2]==0 && rgba[3]==255,
              "DXT1 opaque block");
        // DXT1 punch-through: c0 < c1, pixel 0 index 3 -> transparent black.
        Check(decode_pixels(4,4,"DXT1",{0x1F,0x00,0x00,0xF8,3,0,0,0},rgba) &&
              rgba[0]==0 && rgba[3]==0,"DXT1 punch-through alpha");
        // DXT3: explicit alpha nibbles (pixel0 0xF, pixel1 0x2), red colour.
        Check(decode_pixels(4,4,"DXT3",{0x2F,0,0,0,0,0,0,0,0x00,0xF8,0,0,0,0,0,0},rgba) &&
              rgba[0]==255 && rgba[3]==255 && rgba[7]==34,"DXT3 explicit alpha");
        Check(decode_pixels(1,1,"A8L8",{0x80,0x40},rgba) &&
              rgba==std::vector<uint8_t>({0x40,0x40,0x40,0x80}),"A8L8 luminance+alpha");
        Check(decode_pixels(1,1,"RGBA4444",{0x0F,0xF0},rgba) &&
              rgba==std::vector<uint8_t>({255,0,0,255}),"RGBA4444 expansion");
        Check(decode_pixels(1,1,"RGBA5551",{0xFF,0xFF},rgba) &&
              rgba==std::vector<uint8_t>({255,255,255,255}),"RGBA5551 expansion");
        Check(decode_pixels(1,1,"RGBA5650",{0x00,0xF8},rgba) &&
              rgba==std::vector<uint8_t>({255,0,0,255}),"RGBA5650 expansion");
        Check(decode_pixels(1,1,"RGBX8",{10,20,30,99},rgba) &&
              rgba==std::vector<uint8_t>({10,20,30,255}),"RGBX8 drops the X byte");
        // mip chains append levels after level 0; level 0 is still read, but
        // trailing bytes are rejected when no mip descriptor is present.
        Check(decode_pixels(1,1,"RGBA8",{1,2,3,4,9,9,9,9},rgba,true),"raw level 0 with trailing mips");
        Check(!decode_pixels(1,1,"RGBA8",{1,2,3,4,9,9,9,9},rgba),"trailing bytes without mips rejected");
        Check(decode_pixels(1,1,"RGBA8",{1,2,3,4},rgba) &&
              rgba==std::vector<uint8_t>({1,2,3,4}),"raw exact length still required without mips");
    }
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
    Check(model->SyncFrame("actor","idle")==10,"sync frame derivation follows child motions");
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

    // Artemis script contract: setCoord(x, y, z, angle) and setScale(scale,
    // origin, origin) are instant placements; setColor is 0xRRGGBBAA.
    player.SetCoordAngle(7,8,0,45);
    player.GetCoord(&x,&y);
    Check(x==7 && y==8 && player.GetZ()==0 && player.GetRot()==45,"setCoord angle contract");
    player.SetScaleOrigin(0.5,4,6);
    player.GetScale(&x,&y);
    Check(x==0.5 && y==0.5 && player.GetOriginX()==4 && player.GetOriginY()==6,
          "setScale origin contract");
    player.SetColor(0x112233ffu,0,0);
    Check(player.GetColor()==0x112233ffu,"RGBA color round-trip (opaque)");
    player.SetColor(0x44556600u,0,0);
    Check(player.GetColor()==0x44556600u,"RGBA color round-trip (transparent)");

    // Engine knobs recorded from the shipped scripts.
    player.SetMeshDivisionRatio(0.4);
    player.SetHairScale(0.5);
    player.SetBustScale(0.75);
    Check(player.MeshDivisionRatio()==0.4 && player.HairScale()==0.5 && player.BustScale()==0.75,
          "mesh/hair/bust knobs recorded");

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

    // progress=false (the createEmoteLayer option): the wall clock is ignored
    // until the script calls progress()/step().
    artc::EmotePlayer manual;
    Check(manual.Load(model,error),error.c_str());
    manual.SetAutoProgress(false);
    Check(manual.PlayTimeline("once",1,error),error.c_str());
    Check(manual.Update(0,&compositor,"m") && manual.Update(1000,&compositor,"m"),error.c_str());
    Check(manual.IsTimelinePlaying("once"),"manual progress ignores the wall clock");
    manual.Progress(1000);  // 60 frames → the finite timeline finishes
    Check(!manual.IsTimelinePlaying("once"),"explicit progress advances a manual player");

    // native pivot fold: translate(coord) * scale * translate(-origin)
    artc::EmotePlayer pivoted;
    Check(pivoted.Load(model,error),error.c_str());
    pivoted.SetCoordAngle(0,0,0,0);
    pivoted.SetScaleOrigin(2,1,0);
    Check(pivoted.Render(compositor,"z",error),error.c_str());
    Check(std::abs(compositor.GetLayerInfo("z").left-(-2.0))<1e-6 &&
          std::abs(compositor.GetLayerInfo("z").top-0.0)<1e-6,
          "scale origin folds into the container position");

    // createEmoteLayer's width/height box centres the model origin: the
    // container is placed at (w/2, h/2) before setCoord (reference host
    // model_origin). Regression for the device 立绘 placement.
    artc::EmotePlayer sized;
    Check(sized.Load(model,error),error.c_str());
    sized.SetLayerSize(1920,2048);
    sized.SetCoordAngle(3,4,0,0);
    Check(sized.Render(compositor,"s",error),error.c_str());
    Check(std::abs(compositor.GetLayerInfo("s").left-(963.0))<1e-6 &&
          std::abs(compositor.GetLayerInfo("s").top-(1028.0))<1e-6,
          "layer size centres the model origin");

    // Immediate placements must survive Skip()/Pass(): Finish() snaps to the
    // target, which an instant Set now updates as well (device 立绘 regression:
    // a skip after creation zeroed scale/alpha/coord and hid the sprite).
    artc::EmotePlayer skipped;
    Check(skipped.Load(model,error),error.c_str());
    skipped.SetCoordAngle(7,8,0,45);
    skipped.SetScaleOrigin(0.5,0,0);
    skipped.SetColor(0x404040ffu,0,0);
    skipped.Pass();
    double skip_sx=0,skip_sy=0;
    skipped.GetCoord(&x,&y);
    skipped.GetScale(&skip_sx,&skip_sy);
    Check(x==7 && y==8 && skip_sx==0.5 && skip_sy==0.5 && skipped.GetRot()==45 &&
          skipped.GetColor()==0x404040ffu,"skip/pass keeps immediate placements");

    // E11: SetColor tints the part layers with MODULATE2X (0x80 = neutral);
    // the raw 0xRRGGBBAA value round-trips through GetColor.
    artc::EmotePlayer tinted;
    Check(tinted.Load(model,error),error.c_str());
    tinted.SetColor(0x7f4040ffu,0,0);
    Check(tinted.Render(compositor,"t",error),error.c_str());
    bool tinted_layer=false;
    for(const auto& l:compositor.Layers())
        if(l.id.rfind("t",0)==0 && l.id.size()>=7 &&
           l.id.compare(l.id.size()-7,7,".000000")==0 && l.effect.multiply==0xFE8080u)
            tinted_layer=true;
    Check(tinted_layer,"MODULATE2X color tint reaches the part layers");
    Check(tinted.GetColor()==0x7f4040ffu,"color value round-trips through the tint");

    // WP5: per-frame color multiplies the part tint (MODULATE2X, 0x80 neutral).
    {
        auto color_document=emote_fixture::Scene();
        auto& face_content=color_document.root.object["object"].object["actor"].object["motion"]
            .object["face"].object["layer"].array[0].object["frameList"].array[0].object["content"];
        face_content.object["color"]=N(0x404080ff);
        auto color_model=std::make_shared<artc::EmoteModel>();
        Check(color_model->Load(std::move(color_document),error),error.c_str());
        artc::EmotePlayer colored;
        Check(colored.Load(color_model,error),error.c_str());
        Check(colored.Render(compositor,"w1",error),error.c_str());
        bool tinted_part=false;
        for(const auto& l:compositor.Layers())
            if(l.id.rfind("w1",0)==0 && l.id.size()>=7 &&
               l.id.compare(l.id.size()-7,7,".000000")==0 && l.effect.multiply==0x8080FFu)
                tinted_part=true;
        Check(tinted_part,"frame color multiplies the part tint");
    }

    // WP8: SkipToSync parks the base motion at the derived sync frame.
    artc::EmotePlayer synced;
    Check(synced.Load(model,error),error.c_str());
    synced.Progress(10*1000.0/60.0);  // exactly 10 frames
    Check(synced.Render(compositor,"y",error),error.c_str());
    const float at_sync=compositor.GetLayerInfo("y.000001").left;
    synced.SkipToSync();
    Check(synced.Render(compositor,"y",error),error.c_str());
    Check(compositor.GetLayerInfo("y.000001").left==at_sync,"skipToSync parks at the sync frame");

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

    // WP1: automatic eye blink (metadata.eyeControl). Native phase model:
    // 40% closing / 20% hold / 40% opening at 2.5x; waits are not animation.
    auto blink_document=emote_fixture::Scene();
    blink_document.root.object["metadata"].object["eyeControl"]=emote_fixture::A({
        emote_fixture::O({{"label",emote_fixture::S("expression")},
                          {"beginFrame",emote_fixture::N(0)},
                          {"endFrame",emote_fixture::N(10)},
                          {"blinkFrameCount",emote_fixture::N(4)},
                          {"blinkIntervalMin",emote_fixture::N(10)},
                          {"blinkIntervalMax",emote_fixture::N(10)}})});
    auto blink_model=std::make_shared<artc::EmoteModel>();
    Check(blink_model->Load(std::move(blink_document),error),error.c_str());
    Check(blink_model->Blinks().size()==1 && blink_model->Blinks()[0].variable=="expression" &&
          blink_model->Blinks()[0].frames==4,"eye control parsed");
    artc::EmotePlayer blinker;
    Check(blinker.Load(blink_model,error),error.c_str());
    Check(blinker.Render(compositor,"b",error) && picture_sum("b")==6 && !blinker.IsAnimating(),
          "a pending blink sits at the base value and is not animation");
    blinker.Progress(10*1000.0/60.0);  // interval elapses → closing starts
    blinker.Progress(2*1000.0/60.0);   // 2 frames ≥ the 1.6-frame closing arc
    Check(blinker.Render(compositor,"b",error) && picture_sum("b")==8,
          "blink closes to the end frame");
    blinker.Progress(10*1000.0/60.0);  // hold + open + next wait
    Check(blinker.Render(compositor,"b",error) && picture_sum("b")==6,
          "blink returns to the base value");
    Check(!blinker.IsAnimating(),"a blink cycle never reports animation");
    Check(blinker.Load(blink_model,error),error.c_str());  // reload resets the blink state

    // WP2: selectorControl — option crossfade (selected gets onValue, the
    // blend factor is min(|value-index|,1)); selector labels are settable.
    auto selector_document=emote_fixture::Scene();
    selector_document.root.object["metadata"].object["variableList"].array.push_back(
        emote_fixture::O({{"label",emote_fixture::S("partner")}}));
    selector_document.root.object["metadata"].object["selectorControl"]=emote_fixture::A({
        emote_fixture::O({
            {"label",emote_fixture::S("clothes")},
            {"enabled",emote_fixture::N(1)},
            {"optionList",emote_fixture::A({
                emote_fixture::O({{"label",emote_fixture::S("expression")},
                                  {"onValue",emote_fixture::N(10)},
                                  {"offValue",emote_fixture::N(0)}}),
                emote_fixture::O({{"label",emote_fixture::S("partner")},
                                  {"onValue",emote_fixture::N(1)},
                                  {"offValue",emote_fixture::N(0)}}),
            })},
        }),
    });
    auto selector_model=std::make_shared<artc::EmoteModel>();
    Check(selector_model->Load(std::move(selector_document),error),error.c_str());
    Check(selector_model->Selectors().size()==1 && selector_model->Selectors()[0].items.size()==2,
          "selector control parsed");
    {
        const auto& sel=selector_model->Selectors()[0];
        Check(std::abs(artc::EmotePlayer::SelectorOptionValue(sel,0.0,0)-10)<1e-9 &&
              std::abs(artc::EmotePlayer::SelectorOptionValue(sel,1.0,0)-0)<1e-9 &&
              std::abs(artc::EmotePlayer::SelectorOptionValue(sel,0.5,0)-5)<1e-9 &&
              std::abs(artc::EmotePlayer::SelectorOptionValue(sel,1.0,1)-1)<1e-9,
              "selector crossfade formula");
    }
    artc::EmotePlayer clothes;
    Check(clothes.Load(selector_model,error),error.c_str());
    Check(clothes.Render(compositor,"s",error) && picture_sum("s")==8,
          "selector defaults to option 0 (onValue)");
    Check(clothes.SetVariable("clothes",1,0,0,error),error.c_str());
    Check(clothes.Render(compositor,"s",error) && picture_sum("s")==6,
          "selecting option 1 hands the first option its offValue");

    // WP3: attrcomp removes layers (value<=0) before validation and evaluation.
    auto removal_document=emote_fixture::Scene();
    removal_document.root.object["metadata"].object["attrcomp"]=emote_fixture::A({
        emote_fixture::O({
            {"label",emote_fixture::S("clothes")},
            {"data",emote_fixture::O({
                {"remove",emote_fixture::A({
                    emote_fixture::O({
                        {"value",emote_fixture::N(0)},
                        {"id",emote_fixture::O({{"chara",emote_fixture::S("actor")},
                                                {"motion",emote_fixture::S("idle")},
                                                {"layer",emote_fixture::S("face child")}})},
                    }),
                })},
            })},
        }),
    });
    // The removed node carries an unsupported stencil flag on purpose: the
    // load must still succeed because validation skips removed nodes.
    removal_document.root.object["object"].object["actor"].object["motion"].object["idle"]
        .object["layer"].array[0].object["children"].array[1].object["stencilType"]=emote_fixture::N(1);
    auto removal_model=std::make_shared<artc::EmoteModel>();
    Check(removal_model->Load(std::move(removal_document),error),error.c_str());
    Check(removal_model->Removals().size()==1,"attrcomp removal parsed");
    artc::EmotePlayer removed;
    Check(removed.Load(removal_model,error),error.c_str());
    Check(removed.Render(compositor,"k",error) && picture_sum("k")==4,
          "attrcomp hides the removed node");

    // WP4: metadata.mirror flips by default; Lua setMirror overrides.
    auto mirror_document=emote_fixture::Scene();
    mirror_document.root.object["metadata"].object["mirror"]=emote_fixture::N(1);
    auto mirror_model=std::make_shared<artc::EmoteModel>();
    Check(mirror_model->Load(std::move(mirror_document),error),error.c_str());
    Check(mirror_model->Mirrored(),"mirror metadata parsed");
    artc::EmotePlayer mirrored;
    Check(mirrored.Load(mirror_model,error),error.c_str());
    Check(mirrored.IsMirrored(),"model mirror applies by default");
    mirrored.SetMirror(false);
    Check(!mirrored.IsMirrored(),"Lua mirror overrides the model bit");

    // WP9: `shape/...` (type==1) nodes give hit-test geometry; the SDK figure
    // is a 16x16 unit square scaled/rotated by the node transform.
    auto shape_document=emote_fixture::Scene();
    auto shape_node=emote_fixture::Node(1,"hit",emote_fixture::A({
        emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("shape/rect")},
            {"coord",emote_fixture::A({emote_fixture::N(20),emote_fixture::N(10),emote_fixture::N(0)})},
            {"zx",emote_fixture::N(2)},{"zy",emote_fixture::N(2)}})),
        emote_fixture::Key(11,0)}));
    auto circle_node=emote_fixture::Node(1,"ring",emote_fixture::A({
        emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("shape/circle")},
            {"coord",emote_fixture::A({emote_fixture::N(-20),emote_fixture::N(0),emote_fixture::N(0)})}})),
        emote_fixture::Key(11,0)}));
    shape_document.root.object["object"].object["actor"].object["motion"].object["idle"]
        .object["layer"].array.push_back(shape_node);
    shape_document.root.object["object"].object["actor"].object["motion"].object["idle"]
        .object["layer"].array.push_back(circle_node);
    auto shape_model=std::make_shared<artc::EmoteModel>();
    Check(shape_model->Load(std::move(shape_document),error),error.c_str());
    artc::EmotePlayer shapes;
    Check(shapes.Load(shape_model,error),error.c_str());
    Check(shapes.Render(compositor,"h",error),error.c_str());
    Check(shapes.Contains(compositor,"h","hit",20,10) &&
          !shapes.Contains(compositor,"h","hit",40,10),
          "rect shape hit test uses the scaled node transform");
    Check(shapes.Contains(compositor,"h","ring",-20,0) &&
          !shapes.Contains(compositor,"h","ring",-20,9),
          "circle shape hit test uses the inscribed radius");
    Check(shapes.Contains(compositor,"h","",-20,0) && !shapes.Contains(compositor,"h","missing",-20,0),
          "unlabeled hit tests span every shape, labels filter");

    // WP10: setMeshDivisionRatio scales the warped-grid subdivision (the full
    // quality default 1.0 keeps the 8x8 output).
    {
        auto mesh_document=emote_fixture::Scene();
        artc::PsbValue bp;bp.type=artc::PsbValue::Array;
        for(int j=0;j<4;++j)for(int i=0;i<4;++i) {
            bp.array.push_back(N(double(i)/3.0));
            bp.array.push_back(N(double(j)/3.0));
        }
        auto& face_content=mesh_document.root.object["object"].object["actor"].object["motion"]
            .object["face"].object["layer"].array[0].object["frameList"].array[0].object["content"];
        face_content.object["mesh"]=O({{"bp",bp}});
        auto mesh_model=std::make_shared<artc::EmoteModel>();
        Check(mesh_model->Load(std::move(mesh_document),error),error.c_str());
        artc::EmotePlayer full,coarse;
        Check(full.Load(mesh_model,error) && coarse.Load(mesh_model,error),error.c_str());
        auto mesh_vertices=[&](const char* id) {
            size_t count=0;
            for(const auto& l:compositor.Layers())
                if(l.id.rfind(id,0)==0 && l.id.size()>=7 &&
                   l.id.compare(l.id.size()-7,7,".000000")==0 && !l.mesh.empty())
                    count+=l.mesh.size()/4;
            return count;
        };
        full.SetMeshDivisionRatio(1.0);
        Check(full.Render(compositor,"v1",error),error.c_str());
        coarse.SetMeshDivisionRatio(0.5);
        Check(coarse.Render(compositor,"v2",error),error.c_str());
        // Unindexed triangle list: (side-1)^2 cells * 2 triangles * 3 vertices.
        Check(mesh_vertices("v1")==7*7*6 && mesh_vertices("v2")==3*3*6,
              "mesh division ratio scales the warped grid subdivision");    }

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

    // Native additions recovered from the driver: SetGrayscale animates a
    // luminance blend on the textured scene layers, SetVariableDiff drives a
    // matched pair from one control, and Step advances one 60 fps frame.
    artc::EmotePlayer shading;
    Check(shading.Load(model,error),error.c_str());
    shading.SetGrayscale(1,0,0);
    Check(shading.GetGrayscale()==1,"grayscale round-trip");
    Check(shading.Render(compositor,"g",error),error.c_str());
    bool grayscale_set=false;
    for(const auto& l:compositor.Layers())
        if(l.id.rfind("g",0)==0 && l.id.size()>=7 &&
           l.id.compare(l.id.size()-7,7,".000000")==0 && l.effect.grayscale>0.99f)
            grayscale_set=true;
    Check(grayscale_set,"grayscale reaches the textured scene layer");
    shading.SetGrayscale(0,1000,0);
    shading.Progress(500);
    Check(std::abs(shading.GetGrayscale()-0.5)<1e-9,"grayscale transitions");

    auto pair_document=emote_fixture::PlayerDocument();
    pair_document.root.object["metadata"].object["variableList"].array.push_back(O({{"label",S("partner")}}));
    auto pair_model=std::make_shared<artc::EmoteModel>();
    Check(pair_model->Load(std::move(pair_document),error),error.c_str());
    artc::EmotePlayer pairs;
    Check(pairs.Load(pair_model,error),error.c_str());
    Check(!pairs.SetVariableDiff("expression","missing",5,0,0,error) &&
          error.find("unknown")!=std::string::npos,"a diff pair requires model variables");
    Check(pairs.SetVariableDiff("expression","partner",5,1000,0,error),error.c_str());
    pairs.Progress(500);
    Check(pairs.GetVariable("expression",&found)==2.5 && found &&
          pairs.GetVariable("partner",&found)==-2.5 && found,
          "a difference pair drives the two variables equal and opposite");

    artc::EmotePlayer stepper;
    Check(stepper.Load(model,error),error.c_str());
    Check(stepper.PlayTimeline("once",0,error),error.c_str());
    for(int i=0;i<60;++i) stepper.Step();
    Check(!stepper.IsTimelinePlaying("once"),"step advances one 60 fps frame");

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
    Check(queue.FadeOutTimeline("loop",200,0,error),error.c_str());
    queue.Progress(200);  // blend reaches 0 → auto-stop
    Check(!queue.IsTimelinePlaying("loop") && queue.TimelineBlendRatio("loop",&found)==0 && !found,
          "fade-out removes the timeline at zero blend");
    Check(!queue.FadeOutTimeline("loop",100,0,error) && error.find("not playing")!=std::string::npos,
          "fading an idle timeline fails");
    Check(queue.FadeInTimeline("loop",100,0,error),error.c_str());
    queue.Progress(100);  // fade completes → blend 1
    Check(queue.FadeOutTimeline("loop",1000,0,error),error.c_str());
    queue.Progress(100);  // blend 1 → 0.9
    Check(queue.SetTimelineBlendRatio("loop",0.5,error),error.c_str());
    queue.Progress(2000);
    Check(queue.TimelineBlendRatio("loop",&found)==0.5 && found && queue.IsTimelinePlaying("loop"),
          "a direct blend set cancels the fade and its auto-stop");
    Check(queue.SetTimelineHoldEnd("loop",false,error),error.c_str());
    queue.Progress(1000);  // 60 frames ≥ loop end 20 → parks
    Check(!queue.IsTimelinePlaying("loop"),"hold-end parks a looping timeline at its loop end");

    // The native contract carries a transition: (label, ratio, time, ease, flags).
    artc::EmotePlayer blend;
    Check(blend.Load(model,error),error.c_str());
    Check(blend.PlayTimeline("loop",0,error),error.c_str());
    Check(blend.SetTimelineBlendRatio("loop",0.25,200,0,false,error),error.c_str());
    Check(std::abs(blend.TimelineBlendRatio("loop",&found)-1.0)<1e-9 && found,
          "a blend transition starts from the current ratio");
    blend.Progress(100);
    Check(std::abs(blend.TimelineBlendRatio("loop",&found)-0.625)<1e-9 && found,
          "a blend transition interpolates");
    blend.Progress(100);
    Check(std::abs(blend.TimelineBlendRatio("loop",&found)-0.25)<1e-9 && found,
          "a blend transition lands on the target");
    Check(blend.SetTimelineBlendRatio("loop",0.0,100,0,true,error),error.c_str());
    blend.Progress(100);
    Check(!blend.IsTimelinePlaying("loop"),"a blend transition with flags removes the timeline at zero");

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
    {
        // WP5: newer MotionEditor curve/color content must not reject the model
        // yet (evaluation keeps treating them as absent; a first-use log notes).
        auto curved=emote_fixture::Scene();
        std::string error;
        auto& content=curved.root.object["object"].object["actor"].object["motion"].object["idle"]
            .object["layer"].array[0].object["frameList"].array[0].object["content"];
        content.object["cc"]=emote_fixture::O({{"c",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0)})},
            {"x",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})},
            {"y",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})}});
        content.object["zcc"]=emote_fixture::O({{"c",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0)})},
            {"x",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})},
            {"y",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})}});
        content.object["ccc"]=emote_fixture::O({{"c",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0)})},
            {"x",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})},
            {"y",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0),emote_fixture::N(0)})}});
        content.object["color"]=emote_fixture::N(0xff808080);
        auto curved_model=std::make_shared<artc::EmoteModel>();
        Check(curved_model->Load(curved,error),error.c_str());
        artc::EmoteScene curved_scene;
        Check(curved_scene.Load(curved_model,error),error.c_str());
        std::vector<artc::EmoteSceneLayer> curved_layers;
        Check(curved_scene.Evaluate(5,{},curved_layers,error) && !curved_layers.empty(),
            "curve/color content loads without rejecting the model");
    }
    {
        // Win-split content (甜蜜女友3-style): src names the texture source and
        // icon the entry; type==3 nests (src=chara, icon=motion) and supports a
        // motion-level parameterize; stencil nodes are skipped, null bp/cc mesh
        // placeholders stay affine.
        std::string error;
        artc::PsbValue pixel; pixel.type=artc::PsbValue::Resource; pixel.resource=0;
        artc::PsbValue null_mesh; null_mesh.type=artc::PsbValue::Null;
        auto sub_node=emote_fixture::Node(0,"subpic",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("tex")},
                {"icon",emote_fixture::S("a")},{"coord",emote_fixture::A({emote_fixture::N(1),emote_fixture::N(1),emote_fixture::N(0)})}})),
            emote_fixture::Key(10,2,emote_fixture::O({{"src",emote_fixture::S("tex")},
                {"icon",emote_fixture::S("b")}})),
            emote_fixture::Key(11,0)}));
        auto sub_motion=emote_fixture::O({{"layer",emote_fixture::A({sub_node})},
            {"lastTime",emote_fixture::N(11)},{"loopTime",emote_fixture::N(-1)},
            {"parameterize",emote_fixture::N(0)},
            {"parameter",emote_fixture::A({emote_fixture::O({{"id",emote_fixture::S("expression")},
                {"rangeBegin",emote_fixture::N(0)},{"rangeEnd",emote_fixture::N(10)},
                {"division",emote_fixture::N(10)}})})}});
        auto pic_node=emote_fixture::Node(0,"pic",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("tex")},
                {"icon",emote_fixture::S("a")},
                {"mesh",emote_fixture::O({{"bp",null_mesh},{"cc",null_mesh}})}})),
            emote_fixture::Key(11,0)}));
        auto stencil_node=emote_fixture::Node(12,"stencil",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("tex")},
                {"icon",emote_fixture::S("a")},{"stencilType",emote_fixture::N(4)}})),
            emote_fixture::Key(11,0)}),
            emote_fixture::A({emote_fixture::Node(0,"stencil_child",emote_fixture::A({
                emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("tex")},
                    {"icon",emote_fixture::S("b")}})),
                emote_fixture::Key(11,0)}))}));
        auto sub_ref=emote_fixture::Node(3,"subref",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("body")},
                {"icon",emote_fixture::S("sub")},
                {"motion",emote_fixture::O({{"timeOffset",emote_fixture::N(0)}})}})),
            emote_fixture::Key(11,0)}));
        auto blank_node=emote_fixture::Node(2,"blanky",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("blank")},
                {"icon",emote_fixture::S("4:2:1:0.5")}})),
            emote_fixture::Key(11,0)}));
        artc::PsbDocument split;
        split.root=emote_fixture::O({{"spec",emote_fixture::S("common")},
            {"source",emote_fixture::O({{"tex",emote_fixture::O({
                {"texture",emote_fixture::O({{"width",emote_fixture::N(4)},{"height",emote_fixture::N(2)},
                    {"type",emote_fixture::S("RGBA8")},{"pixel",pixel}})},
                {"icon",emote_fixture::O({
                    {"a",emote_fixture::O({{"left",emote_fixture::N(0)},{"top",emote_fixture::N(0)},
                        {"width",emote_fixture::N(2)},{"height",emote_fixture::N(2)},
                        {"originX",emote_fixture::N(1)},{"originY",emote_fixture::N(1)}})},
                    {"b",emote_fixture::O({{"left",emote_fixture::N(2)},{"top",emote_fixture::N(0)},
                        {"width",emote_fixture::N(2)},{"height",emote_fixture::N(2)},
                        {"originX",emote_fixture::N(1)},{"originY",emote_fixture::N(1)}})}})}})}})},
            {"object",emote_fixture::O({{"body",emote_fixture::O({{"motion",emote_fixture::O({
                {"idle",emote_fixture::O({{"layer",emote_fixture::A({pic_node,stencil_node,sub_ref,blank_node})},
                    {"lastTime",emote_fixture::N(11)},{"loopTime",emote_fixture::N(0)}})},
                {"sub",sub_motion}})}})}})},
            {"metadata",emote_fixture::O({{"base",emote_fixture::O({{"chara",emote_fixture::S("body")},
                {"motion",emote_fixture::S("idle")}})},
                {"variableList",emote_fixture::A({emote_fixture::O({{"label",emote_fixture::S("expression")}})})}})}});
        for(int i=0;i<8;++i) {
            split.bytes.push_back(uint8_t(10+i*10));
            split.bytes.push_back(uint8_t(20+i*10));
            split.bytes.push_back(uint8_t(30+i*10));
            split.bytes.push_back(255);
        }
        split.resources={{0,32}};
        auto split_model=std::make_shared<artc::EmoteModel>();
        Check(split_model->Load(split,error),error.c_str());
        artc::EmoteScene split_scene;
        Check(split_scene.Load(split_model,error),error.c_str());
        std::vector<artc::EmoteSceneLayer> split_layers;
        Check(split_scene.Evaluate(0,{{"expression",0}},split_layers,error),"split model evaluates");
        std::string sub_icon;bool stencil_own_draw=false;bool stencil_child=false;bool pic_found=false;
        for(const auto& l:split_layers) {
            if(l.label=="subpic")sub_icon=l.icon;
            if(l.label=="stencil" && !l.source.empty())stencil_own_draw=true;
            if(l.label=="stencil_child" && l.source=="tex" && l.icon=="b")stencil_child=true;
            if(l.label=="pic" && l.source=="tex" && l.icon=="a")pic_found=true;
        }
        Check(pic_found && !stencil_own_draw && stencil_child,
              "split content draws by source+icon; stencil keeps its children");
        Check(sub_icon=="a","nested split motion evaluates at parameter 0");
        bool blank_found=false;
        for(const auto& l:split_layers)
            if(l.label=="blanky" && l.origin_x==1 && l.origin_y==0.5)blank_found=true;
        Check(blank_found,"blank descriptor sets the layout origin");
        Check(split_scene.Evaluate(0,{{"expression",10}},split_layers,error),"split model evaluates at 10");
        sub_icon.clear();
        for(const auto& l:split_layers) if(l.label=="subpic")sub_icon=l.icon;
        Check(sub_icon=="b","motion-level parameterize selects the nested tick");
    }
    {
        // E9: motion.priority reorders sibling emission (draw order). The
        // content list is reverse draw order (last entry draws first), so
        // [0,1] puts node 1 (pb) at the bottom.
        auto prio_doc=emote_fixture::Scene();
        auto node_a=emote_fixture::Node(0,"pa",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("src/images/face")}})),
            emote_fixture::Key(11,0)}));
        auto node_b=emote_fixture::Node(0,"pb",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("src/images/body")}})),
            emote_fixture::Key(11,0)}));
        auto& idle=prio_doc.root.object["object"].object["actor"].object["motion"].object["idle"];
        idle.object["layer"]=emote_fixture::A({node_a,node_b});
        idle.object["priority"]=emote_fixture::A({
            emote_fixture::O({{"time",emote_fixture::N(0)},
                {"content",emote_fixture::A({emote_fixture::N(0),emote_fixture::N(1)})}})});
        auto prio_model=std::make_shared<artc::EmoteModel>();
        std::string error;
        Check(prio_model->Load(prio_doc,error),error.c_str());
        artc::EmoteScene prio_scene;
        Check(prio_scene.Load(prio_model,error),error.c_str());
        std::vector<artc::EmoteSceneLayer> prio_layers;
        Check(prio_scene.Evaluate(0,{},prio_layers,error),"priority model evaluates");
        Check(prio_layers.size()>=2 && prio_layers[0].label=="pb" && prio_layers[1].label=="pa",
              "priority content reorders sibling emission");
    }
    {
        // E9 (device evidence): content `bm` selects the native blend mode and
        // non-default inheritMask masks load; both appear in real 立绘 models.
        auto blend_doc=emote_fixture::Scene();
        auto& blend_idle=blend_doc.root.object["object"].object["actor"].object["motion"].object["idle"];
        auto& group=blend_idle.object["layer"].array[0];
        group.object["children"].array[0].object["frameList"].array[0].object["content"]
            .object["bm"]=emote_fixture::N(0x13);
        auto masked=emote_fixture::Node(2,"masked",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("src/images/face")},
                {"bm",emote_fixture::N(2)}})),
            emote_fixture::Key(11,0)}));
        masked.object["inheritMask"]=emote_fixture::N(0x200020c);
        group.object["children"].array.push_back(masked);
        auto blend_model=std::make_shared<artc::EmoteModel>();
        std::string error;
        Check(blend_model->Load(blend_doc,error),error.c_str());
        artc::EmoteScene blend_scene;
        Check(blend_scene.Load(blend_model,error),error.c_str());
        std::vector<artc::EmoteSceneLayer> blend_layers;
        Check(blend_scene.Evaluate(0,{},blend_layers,error),"blend model evaluates");
        bool multiply=false,subtract=false;
        for(const auto& l:blend_layers) {
            if(l.blend=="multiply")multiply=true;
            if(l.blend=="subtract")subtract=true;
        }
        Check(multiply && subtract,"content bm maps to native blend modes");
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
    // A forged encryption flag with a plain body fails header validation (the
    // real encrypted-header path is covered by psb_encrypted_regressions).
    Check(!artc::DecodePsb(broken,doc,error) && !error.empty(),"forged encrypted PSB is rejected");
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
