#include "audio/audio_channels.h"
#include "config/ini.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "script/asb_parser.h"
#include "script/expression.h"
#include "script/native_save.h"
#include "script/save_storage.h"
#include "emote_scene_fixture.h"
#include <zlib.h>
#include "render/compositor.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#if defined(ARTC_HAS_GLES)
#include <EGL/egl.h>
#endif

static void Check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}

class Output : public artc::Audio {
public:
    struct Voice { std::string file; int gain, pan = 0; bool playing = true; };
    std::map<std::string, Voice> voices;
    bool Play(const std::string& k, const std::string& f, bool, int g) override {
        if (f == "missing") return false;
        voices[k] = {f, g}; return true;
    }
    void Stop(const std::string& k) override { voices.erase(k); }
    bool IsPlaying(const std::string& k) const override {
        auto v = voices.find(k); return v != voices.end() && v->second.playing;
    }
    void SetVolume(const std::string& k, int gain) override { voices.at(k).gain = gain; }
    void SetPan(const std::string& k, int pan) override { voices.at(k).pan = pan; }
    Voice& File(const std::string& file) {
        for (auto& v : voices) if (v.second.file == file) return v.second;
        std::abort();
    }
};

int main(int argc, char** argv) {
#if defined(ARTC_HAS_GLES)
    // The E-mote Lua block renders through SetPixels, which uploads through
    // real GL; OHOS builds the compositor with GLES unconditionally, so give
    // the binary a pbuffer context before any engine sees a compositor.
    EGLDisplay display=eglGetDisplay(EGL_DEFAULT_DISPLAY);
    Check(eglInitialize(display,nullptr,nullptr),"initialize EGL");
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
    {
    {
        artc::VariableBank input={{"binary",std::string("a\0b",3)},{"empty",""}},output;
        for(bool checkpoint:{false,true}) {
            std::vector<uint8_t> bytes;
            Check(artc::EncodeVariableBank(input,checkpoint,bytes) && artc::DecodeVariableBank(bytes,checkpoint,output) && input==output,
                  "variable banks preserve empty values and binary strings");
            for(size_t n=0;n<bytes.size();++n) {
                const auto broken=std::vector<uint8_t>(bytes.begin(),bytes.begin()+n);
                Check(!artc::DecodeVariableBank(broken,checkpoint,output) && output==input,
                      "truncated bank cannot partially replace live variables");
            }
            if(checkpoint){bytes.back()^=1;Check(!artc::DecodeVariableBank(bytes,true,output),"checkpoint checksum");}
        }
    }
    // Synthetic native CSerializer directory: fields deliberately not ordered.
    std::vector<uint8_t> payload;
    auto u32=[&](uint32_t n){for(int i=0;i<4;++i){payload.push_back(n&255);n>>=8;}};
    auto str=[&](const std::string& v){u32(v.size());payload.insert(payload.end(),v.begin(),v.end());};
    u32(0); // root field 24: empty text map
    const auto layers=payload.size();u32(1);str("1");u32(1);
    u32(0xffffffff);str("lyprop");u32(2);str("id");str("1");str("left");str("42");
    const auto bank=payload.size();u32(1);str("scr");str(std::string("a\0b",3));
    const auto directory=payload.size();u32(2);
    u32(bank);u32(1);u32(2);u32(bank);
    u32(0);u32(3);u32(30);u32(bank);u32(26);u32(layers);u32(24);u32(0);
    u32(directory);
    std::vector<uint8_t> native={'B','O','W','S',0xeb,3,0,0};
    for(int i=0;i<4;++i)native.push_back((payload.size()>>(8*i))&255);
    uLongf compressed=compressBound(payload.size());native.resize(12+compressed);
    Check(compress2(native.data()+12,&compressed,payload.data(),payload.size(),6)==Z_OK,"compress synthetic native save");
    native.resize(12+compressed);
    artc::NativeSave decoded;std::string save_error;
    Check(artc::DecodeNativeSave(native,decoded,save_error),save_error.c_str());
    Check(decoded.variables.at("scr")==std::string("a\0b",3) && decoded.layers.size()==1 &&
          decoded.layers[0].attrs.at("left")=="42","decode indexed native variables and command journal");
    for(size_t i=0;i<native.size();++i) {
        auto truncated=std::vector<uint8_t>(native.begin(),native.begin()+i);
        Check(!artc::DecodeNativeSave(truncated,decoded,save_error),"truncated snapshot rejected");
        Check(decoded.variables.at("scr")==std::string("a\0b",3),"failed snapshot leaves output unchanged");
    }
    auto corrupt=native;corrupt.back()^=1;
    Check(!artc::DecodeNativeSave(corrupt,decoded,save_error),"native checksum verified");
    corrupt=native;corrupt.push_back(0);
    Check(!artc::DecodeNativeSave(corrupt,decoded,save_error),"native compressed trailing data rejected");
    corrupt=native;corrupt[4]=0;
    Check(!artc::DecodeNativeSave(corrupt,decoded,save_error),"unsupported native version rejected");
    // BOWG stores globals directly at the root, without the BOWS field-30 bank.
    payload.clear();u32(1);str("system/first.iet");u32(2);u32(7);u32(3);
    const auto globals_at=payload.size();u32(1);str("g.config");str(std::string("x\0y",3));
    const auto globals_dir=payload.size();u32(1);u32(0);u32(2);
    u32(2);u32(globals_at);u32(1);u32(0);u32(globals_dir);
    std::vector<uint8_t> global={'B','O','W','G',0xeb,3,0,0};
    for(int i=0;i<4;++i)global.push_back((payload.size()>>(8*i))&255);
    compressed=compressBound(payload.size());global.resize(12+compressed);
    Check(compress2(global.data()+12,&compressed,payload.data(),payload.size(),6)==Z_OK,"compress synthetic global bank");
    global.resize(12+compressed);
    artc::NativeGlobals globals;
    Check(artc::DecodeNativeGlobals(global,globals,save_error),save_error.c_str());
    Check(globals.variables.at("g.config")==std::string("x\0y",3) &&
          globals.read_lines.at("system/first.iet")==std::vector<uint32_t>({7,3}),"BOWG globals and read-line sets");
    for(size_t i=0;i<global.size();++i) {
        auto truncated=std::vector<uint8_t>(global.begin(),global.begin()+i);
        Check(!artc::DecodeNativeGlobals(truncated,globals,save_error) && globals.variables.at("g.config")==std::string("x\0y",3),
              "invalid global bank cannot partially replace output");
    }
    Check(!artc::DecodeNativeGlobals(native,globals,save_error) && !artc::DecodeNativeSave(global,decoded,save_error),
          "global and scenario saves cannot be interchanged");
    const auto slot_dir=std::filesystem::temp_directory_path()/
        ("artemis-native-slot-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(slot_dir);
    {std::ofstream f(slot_dir/"slot.dat",std::ios::binary);f.write(reinterpret_cast<const char*>(native.data()),native.size());}
    Check(artc::WriteSaveFile((slot_dir/"saveg.dat").string(),global),"prepare native globals");
    artc::PackManager slot_packs;
    artc::LuaEngine slot_engine;slot_engine.SetSaveDir(slot_dir.string());
    Check(slot_engine.Init(&slot_packs,artc::Ini{},"android",32,32,nullptr),"initialize slot importer");
    Check(slot_engine.DoString("assert(e:var('g.config')=='x\\0y')","native globals available before boot"),
          "first boot imports BOWG globals");
    Check(!slot_engine.LoadSnapshot("slot.dat"),"native import requires an onLoad restorer");
    Check(slot_engine.DoString(R"(
        restored=0
        function restore_slot(e,p)
            assert(p.file=="slot.dat" and e:var("scr")=="a\0b")
            assert(e:var("g.keep")=="7" and e:var("s.keep")=="9")
            restored=restored+1
        end
        e:setEventHandler{onLoad="restore_slot"}
        e:tag{"var",name="g.keep",data="7"};e:tag{"var",name="s.keep",data="9"}
        e:enqueueTag{"exit"}
    )","slot callback"),"configure native restorer");
    slot_engine.SetTimedWait(60000);
    Check(slot_engine.LoadSnapshot("slot.dat"),"import native slot with onLoad");
    Check(slot_engine.DoString("assert(restored==1)","restored"),"onLoad fires once after variables");
    Check(!slot_engine.HasQueuedTag() && !slot_engine.IsWaiting(),
          "load discards the previous queue and wait");
    Check(!slot_engine.LoadSnapshot("../slot.dat"),"slot cannot escape save directory");
    {std::ofstream f(slot_dir/"slot.dat",std::ios::binary);f.write(reinterpret_cast<const char*>(native.data()),12);}
    Check(!slot_engine.LoadSnapshot("slot.dat"),"broken snapshot rejected before mutation");
    Check(slot_engine.DoString("assert(restored==1 and e:var('scr')=='a\\0b')","failed load"),"failed load leaves variables and callback untouched");
    Check(slot_engine.DoString(R"(
        function checkpoint_save(e,p)
            e:tag{"var",name="scr",data=pluto.persist({},{slot=p.file})}
        end
        function checkpoint_load(e,p)
            assert(pluto.unpersist({},e:var("scr")).slot==p.file)
            restored=restored+1
        end
        e:setEventHandler{onSave="checkpoint_save",onLoad="checkpoint_load"}
        e:tag{'var',name='g.config',data='new settings'}
        e:tag{"save",file="one.dat"};e:tag{"save",file="two.dat"}
        assert(e:isFileExists(e:var('s.savepath')..'/one.dat'))
        assert(e:isFileExists(e:var('s.savepath')..'/two.dat'))
    )","checkpoint slots"),"save honors distinct slot filenames and onSave preparation");
    Check(slot_engine.LoadSnapshot("one.dat") && slot_engine.LoadSnapshot("two.dat"),"each checkpoint reloads its own graph");
    Check(!slot_engine.SaveSnapshot("system.dat") && !slot_engine.SaveSnapshot("saveg.dat"),"scenario saves cannot overwrite global banks");
    std::vector<uint8_t> original_global;
    Check(artc::ReadSaveFile((slot_dir/"saveg.dat").string(),original_global) && original_global==global,"native bank is left byte-for-byte intact");
    Check(slot_engine.DoString("assert(restored==3)","checkpoint callbacks"),"checkpoint load callbacks complete");
    Check(slot_engine.DoString(R"(
        local slots={[1]={file='one',date={}},[2]={file='two',date={2000,1,2,3,4,5}},[3]={file='missing',date={}}}
        e:tag{'var',name='g.system',data=pluto.persist({},{saveslot=slots})}; e:tag{'save'}
    )","legacy empty save date"),"persist metadata from an early compatibility build");
    artc::LuaEngine recovered;recovered.SetSaveDir(slot_dir.string());
    Check(recovered.Init(&slot_packs,artc::Ini{},"android",1280,720),"reload bank with missing dates");
    Check(recovered.DoString(R"(
        assert(e:var('g.config')=='new settings')
        local slots=pluto.unpersist({},e:var('g.system')).saveslot
        assert(#slots[1].date==6 and slots[1].date[1]>=1970)
        assert(slots[2].date[1]==2000 and #slots[3].date==0)
    )","migrated date"),"recover a matching ARCV date without altering valid dates or inventing missing-file dates");
    std::filesystem::remove_all(slot_dir);
    // Optional private, read-only original snapshots; never included in fixtures.
    for(int i=1;i<argc;++i) {
        std::ifstream file(argv[i],std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(file)),{});
        if(bytes.size()>=4 && std::string(bytes.begin(),bytes.begin()+4)=="BOWG") {
            Check(artc::DecodeNativeGlobals(bytes,globals,save_error),save_error.c_str());
            std::cout<<"native globals: "<<globals.variables.size()<<" variables, "<<globals.read_lines.size()<<" read-line sets\n";
            const auto directory=slot_dir/"private-global";
            std::filesystem::create_directories(directory);
            Check(artc::WriteSaveFile((directory/"saveg.dat").string(),bytes),"copy private global bank into isolated test directory");
            artc::LuaEngine imported;imported.SetSaveDir(directory.string());
            Check(imported.Init(&slot_packs,artc::Ini{},"android",1280,720),"initialize original global bank");
            Check(imported.DoString(R"(
                for _,name in ipairs{'g.system','g.config','g.script'} do
                    assert(type(pluto.unpersist({},e:var(name)))=='table')
                end
                local slots=pluto.unpersist({},e:var('g.system')).saveslot
                local count=0
                for n,slot in pairs(slots) do if type(n)=='number' and type(slot)=='table' then count=count+1 end end
                assert(count>0)
            )","original global graphs"),"original settings and save slots deserialize before game boot");
            std::filesystem::remove_all(slot_dir);
            continue;
        }
        Check(artc::DecodeNativeSave(bytes,decoded,save_error),save_error.c_str());
        std::cout<<"native snapshot: "<<decoded.variables.size()<<" variables, "
                 <<decoded.layers.size()<<" layer commands, "<<decoded.text.size()<<" text layers\n";
    }
    }
    const auto variable=[](const std::string& name) {
        if(name=="t.count") return std::string("8");
        if(name=="t.path") return std::string("movie/logo.mp4");
        return std::string("0");
    };
    std::string expression;
    const std::pair<const char*,const char*> expressions[]={
        {"1==1","1"},{"1==0","0"},{"2+3*4","14"},{"(2+3)*4","20"},
        {"!0 && (3>=2 || 0)","1"},{"0 || 1 && 0","0"},{"8 & 3 | 2","2"},
        {"0x10 >> 2","4"},{"-7/2","-3"},{"0xffffffff+2","1"},
        {"t.count + $t.count","16"},{"t.path","movie/logo.mp4"},
        {"t.path == 'movie/logo.mp4'","1"},{"'movie/' + 'logo.mp4'","movie/logo.mp4"},
        {"0 && 1/0","0"},{"1 || 1/0","1"}};
    for(auto test:expressions)
        Check(artc::EvaluateExpression(test.first,variable,expression) && expression==test.second,
              test.first);
    for(const char* invalid:{"", "1/0", "(1+2", "1 +", "0x", "1 << 32", "f()", "1;2"})
        Check(!artc::EvaluateExpression(invalid,variable,expression),"invalid expression must fail without execution");
    Output out;
    artc::AudioChannels sounds(out);
    Check(sounds.Play("bgm", "title", true, 800, 0, 0), "play BGM");
    Check(sounds.Play("1", "voice", false, 1000, 0, 0), "play voice");
    Check(sounds.Play("bgm", "story", true, 600, 1000, 0, true), "crossfade BGM");
    sounds.Update(500);
    Check(out.File("title").gain == 400 && out.File("story").gain == 300,
          "crossfade must retain both tracks and interpolate them");
    Check(out.File("voice").gain == 1000, "crossfade must not change voice gain");
    sounds.Update(1000);
    Check(out.voices.size() == 2, "old BGM must retire at fade completion");
    sounds.Stop("1", 0, 1000);
    Check(sounds.IsPlaying("bgm") && !sounds.IsPlaying("1"), "SE stop must isolate channel");
    Check(!sounds.Play("bgm", "missing", true, 1000, 0, 1000, true), "missing BGM fails");
    Check(sounds.IsPlaying("bgm"), "failed replacement must retain current BGM");
    sounds.Fade("bgm", 0, 1000, 1000);
    sounds.Update(1500);
    Check(out.File("story").gain == 300, "gain fade midpoint");
    sounds.Fade("bgm", 900, 1000, 1500);
    sounds.Pan("bgm", -1000, 1000, 1500);
    sounds.Update(2000);
    Check(out.File("story").gain == 600 && out.File("story").pan == -500,
          "retargeting starts from interpolated gain; pan is independent");
    out.File("story").playing = false;
    sounds.Update(2100);
    Check(out.voices.empty() && !sounds.IsPlaying("bgm"), "completed output resources are released");

    artc::Compositor animation;
    animation.BeginTweenSet();
    animation.AddTween("1",{{"param","top"},{"from","0"},{"to","10"},{"time","120"}},0);
    animation.AddTween("1",{{"param","top"},{"from","10"},{"to","5"},{"time","60"}},0);
    animation.AddTween("1",{{"param","top"},{"from","5"},{"to","0"},{"time","60"}},0);
    animation.EndTweenSet(0);
    Check(animation.PendingAnimationMs(0)==240,"nod animation retains all three durations");
    animation.Update(60);
    Check(animation.GetLayerInfo("1").top==5,"nod rises in its first segment");
    animation.Update(150);
    Check(animation.GetLayerInfo("1").top==7.5,"nod executes its middle segment");
    animation.Update(240);
    Check(animation.GetLayerInfo("1").top==0 && animation.PendingAnimationMs(240)==0,
          "nod returns to its original position");
    animation.AddTween("1",{{"param","top"},{"from","0"},{"to","-15"},
                           {"time","100"},{"yoyo","1"}},300);
    animation.Update(400);
    Check(animation.GetLayerInfo("1").top==-15,"yoyo reaches the turning point");
    animation.Update(450);
    Check(animation.GetLayerInfo("1").top==-7.5,"yoyo traverses back");
    animation.Update(500);
    Check(animation.GetLayerInfo("1").top==0,"yoyo finishes at its origin");
    animation.BeginTweenSet();
    animation.AddTween("1",{{"param","left"},{"from","0"},{"to","20"},{"time","100"}},600);
    animation.AddTween("1",{{"param","left"},{"to","40"},{"time","100"}},600);
    animation.AddTween("1",{{"param","top"},{"from","0"},{"to","30"},{"time","100"}},600);
    animation.EndTweenSet(600);
    animation.Update(750);
    Check(animation.GetLayerInfo("1").left==30 && animation.GetLayerInfo("1").top==30,
          "sequences resolve implicit starts after earlier segments; independent properties run together");
    animation.AddTween("2",{{"param","top"},{"from","0"},{"to","20"},
                           {"time","100"},{"loop","-1"}},800);
    animation.Update(1050);
    Check(animation.GetLayerInfo("2").top==10,"infinite loop handles frames spanning several traversals");
    animation.DeleteTweens("2");
    animation.Update(1100);
    Check(animation.PendingAnimationMs(1100)==0,"deleting a loop releases animation wait");

    artc::LuaEngine lua;
    artc::PackManager packs;
    artc::Ini ini;
    Check(lua.Init(&packs, ini, "android", 1280, 720), "initialize Lua");
    Check(lua.DoString(R"(
        e:tag{'var',name='t.stamp',system='date'}
        local n={};for _,field in ipairs{'year','month','day','hour','minute','second'} do
            n[#n+1]=assert(tonumber(e:var('t.stamp.'..field)))
        end
        assert(n[1]>=1970 and n[2]>=1 and n[2]<=12 and n[3]>=1 and n[3]<=31)
        assert(n[4]>=0 and n[4]<=23 and n[5]>=0 and n[5]<=59 and n[6]>=0 and n[6]<=60)
        assert(string.format('%04d/%02d/%02d %02d:%02d',unpack(n)))
        local data=pluto.persist({},n);local restored=pluto.unpersist({},data)
        assert(#restored==6 and restored[1]==n[1])
    )","save timestamp"),"save calendar fields survive Pluto and can format a slot caption");
    Check(lua.DoString("e:tag{'var',name='t.cond',data='$1==1'}; assert(tonumber(e:var('t.cond'))==1); "
        "e:tag{'var',name='t.cond',data='$1==0'}; assert(tonumber(e:var('t.cond'))==0); "
        "e:tag{'var',name='t.count',data=2}; e:tag{'var',name='t.count',data='$t.count + 1'}; "
        "assert(e:var('t.count')=='3'); "
        "local data='a'..string.char(0)..'b'; e:tag{'var',name='save.binary',data=data}; "
        "assert(e:var('save.binary')==data); e:tag{'var',name='save.binary',system='delete'}; "
        "assert(e:var('save.binary')=='')", "variable expressions"),
        "script conditions compute numbers and variable banks retain binary strings");
    lua.DispatchTag("wait",{{"time","$t.count*1000"},{"input","0"}});
    Check(lua.IsWaiting(),"tag attributes resolve expressions before their command consumes them");
    lua.SetWaiting(false);
    lua.PushKeyDown(13);
    Check(lua.DoString("assert(e:isDown(13) and e:isDownEdge(13) and e:isPush(13)); "
        "e:overrideKey{key=13,status=0}; assert(not e:isDown(13)); "
        "e:overrideKey{key=13,status=-1}; assert(e:isDown(13)); "
        "e:overrideKey{status=0}; assert(not e:isDown(13)); "
        "e:overrideKey{key=319,status=32}; assert(e:isDecide(319)); "
        "assert(not e:isDown(319) and not e:isDownEdge(319)); "
        "e:overrideKey{key=13,status=8}; assert(e:isDownEdge(13) and not e:isDown(13)); "
        "e:overrideKey{key=14,status=16}; assert(e:isUpEdge(14)); "
        "e:overrideKey{key=15,status=4}; assert(e:isDown(15) and not e:isPush(15)); "
        "e:overrideKey{key=16,status=2}; assert(e:isPush(16) and not e:isDownEdge(16))",
        "override flags"),"override query bits match the original input API");
    lua.EndFrame();
    Check(lua.DoString("assert(e:isDown(13) and not e:isDownEdge(13)); "
        "assert(not e:isDecide(319) and not e:isUpEdge(14))", "next frame"),
        "frame completion removes overrides and preserves physical held keys");
    lua.PushKeyUp(13);lua.EndFrame();
    lua.DispatchTag("wait", {{"time", "10000"}, {"input", "0"}});
    lua.ClickAt(20, 20); lua.RunEnterFrame();
    Check(lua.IsWaiting(), "mandatory wait must survive pointer input");
    Check(lua.DoString("assert(e:getScriptWaitReason().time)", "wait reason"), "time wait reason");
    lua.SetWaiting(false);
    lua.DispatchTag("wait", {{"time", "10000"}, {"input", "1"}});
    lua.ClickAt(20, 20); lua.RunEnterFrame();
    Check(!lua.IsWaiting(), "skippable timed wait accepts a tap");
    lua.DispatchTag("wait", {{"input", "1"}});
    Check(!lua.IsWaiting(), "input permission without a time/animation must not create a click barrier");
    lua.DispatchTag("wait", {{"input", "1"}, {"time", "0"}});
    Check(!lua.IsWaiting(), "repeated completed waits must fall through");
    lua.DispatchTag("@", {});
    Check(lua.IsWaiting(), "click wait starts");
    Check(lua.DoString("assert(next(e:getScriptWaitReason()) == nil)", "click reason"), "click wait has no timer reason");
    lua.ClickAt(20, 20); lua.RunEnterFrame();
    Check(!lua.IsWaiting(), "click wait ends");

    // Kernel announce protocol. vsync.lua's keyClickEnd drops flg.waitflag
    // only while getScriptWaitReason() still reports a reason at OUT time:
    //   if getWaitStatus() then flg.waitflag = nil end
    // A stale waitflag pins getGameMode("all") at "wait" and keyevent.lua
    // converts every later click into a dummy exclick — title/route buttons
    // dead (observed as START→character-route input loss on the target
    // title). The lazy IsWaiting() poll that
    // used to open l_getScriptWaitReason re-entered the expiry transition
    // mid-announce and wiped the reason before the handler could read it.
    artc::LuaEngine announce;
    Check(announce.Init(&packs, ini, "android", 1280, 720), "announce engine init");
    Check(announce.DoString(R"(
        in_reasons = nil; out_reasons = nil
        local function keys() local t, n = e:getScriptWaitReason(), {}
            for k in pairs(t) do n[#n+1] = k end return #n end
        function probe_wait_in(e, p) in_reasons = keys() end
        function probe_wait_out(e) out_reasons = keys() end
        e:setEventHandler{onClickWaitIn='probe_wait_in',
                          onClickWaitOut='probe_wait_out'}
    )", "announce handlers"), "install wait announce handlers");
    announce.DispatchTag("wait", {{"time", "30"}, {"input", "0"}});
    Check(announce.IsWaiting(), "timed wait holds the runner");
    Check(announce.DoString("assert(in_reasons and in_reasons >= 1)",
                            "in reason readable"),
          "onClickWaitIn reads the timed-wait reason");
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    Check(!announce.IsWaiting(), "expired timed wait releases the runner");
    Check(announce.DoString("assert(out_reasons and out_reasons >= 1)",
                            "out reason readable"),
          "onClickWaitOut still reads the reason so keyClickEnd clears waitflag");
    announce.DispatchTag("@", {});
    Check(announce.IsWaiting(), "plain click wait holds the runner");
    Check(announce.DoString("assert(in_reasons == 0)", "plain in reason"),
          "plain click wait announces with no reason (flg.click routing)");
    announce.ClickAt(20, 20); announce.RunEnterFrame();
    Check(!announce.IsWaiting(), "plain click wait ends");
    Check(announce.DoString("assert(out_reasons == 0)", "plain out reason"),
          "plain click wait OUT also reports no reason");

    Check(lua.DoString("function mask(e) e:overrideKey{status=0} end; "
        "e:setEventHandler{onEnterFrame='mask'}", "mask input"),"install frame input filter");
    lua.SetWaiting(true);lua.ClickAt(20,20);lua.RunEnterFrame();
    Check(lua.IsWaiting(),"vsync can suppress a queued tap before it advances the scenario");
    lua.EndFrame();
    Check(lua.DoString("e:tag{'keyconfig',role=0,keys='124'}; "
        "function mask(e) e:overrideKey{status=0}; e:overrideKey{key=124,status=32} end",
        "virtual input"),"configure virtual confirm role");
    lua.RunEnterFrame();
    Check(!lua.IsWaiting(),"a virtual Decide key advances the configured click role");
    lua.EndFrame();
    Check(lua.DoString("e:setEventHandler{onEnterFrame=''}", "remove filter"),"remove frame filter");

    artc::AutoReadTimer reading;
    Check(!reading.Ready(0, 100, true), "voice/text blocks the auto timer");
    Check(!reading.Ready(500, 100, false), "reading interval starts after voice/text");
    Check(!reading.Ready(599, 100, false), "auto respects the configured interval");
    const double elapsed = reading.Elapsed(550);
    reading.Reset(); reading.Restore(2000, elapsed);
    Check(!reading.Ready(2049, 100, false) && reading.Ready(2050, 100, false),
          "menu time does not consume the suspended reading interval");
    Check(!reading.Ready(2060, 100, true) && !reading.Ready(3000, 100, false),
          "a newly synchronized voice resets the reading interval");

    artc::LuaEngine auto_script;
    Check(auto_script.Init(&packs, ini, "android", 1280, 720), "auto engine init");
    Check(auto_script.DoString("auto_out=0; function auto_stop(e,p) auto_out=auto_out+1 end; "
        "e:tag{'setonautomodeout',['function']='auto_stop'}; "
        "e:tag{'automode',allow=0}; e:tag{'exec',command='automode',mode=1}; "
        "assert(e:var('s.status.automode')=='0'); e:tag{'automode',allow=1}; "
        "e:tag{'exec',command='automode',mode=1}; e:tag{'var',name='s.automodewait',data=0}; "
        "assert(e:var('s.status.automode')=='1')", "auto setup"), "auto permission and state");
    auto_script.SetTimedWait(10000);
    Check(auto_script.IsWaiting(), "auto must not bypass a mandatory timer");
    auto_script.SetWaiting(false); auto_script.SetWaiting(true);
    Check(!auto_script.IsWaiting(), "zero-delay auto releases a click wait");
    Check(auto_script.DoString("e:tag{'var',name='s.automodewait',data=10000}", "auto interval"),
          "set reading interval");
    auto_script.SetWaiting(true); auto_script.ClickAt(10,10); auto_script.RunEnterFrame();
    Check(auto_script.IsWaiting(), "first click cancels auto without skipping the page");
    Check(auto_script.DoString("assert(auto_out==1 and e:var('s.status.automode')=='0'); "
        "e:tag{'exec',command='automode',mode=0}; assert(auto_out==1)", "auto stopped"),
        "auto stop callback fires exactly once");
    auto_script.ClickAt(10,10); auto_script.RunEnterFrame();
    Check(!auto_script.IsWaiting(), "next click advances after cancelling auto");

    artc::Compositor compositor;
    compositor.SetProps("500.1", {{"w", "100"}, {"h", "100"}});
    const_cast<artc::Layer&>(compositor.Layers().front()).texture = 1;
    // Separate Lua instance avoids depending on game-global button tables.
    artc::LuaEngine events;
    Check(events.Init(&packs, ini, "android", 1280, 720, &compositor), "event engine init");
    Check(events.DoString("calls=0; function button(e,p) calls=calls+1 end; "
                          "e:setEventFilter(function(e,kind,p) return 1 end); "
                          "e:tag{'lyevent',id='500.1',type='click',handler='calllua',['function']='button'}",
                          "event setup"), "set filtered layer event");
    events.SetWaiting(true);
    events.ClickAt(50,50); events.RunEnterFrame();
    Check(events.IsWaiting(), "filtered button must not release the script wait");
    Check(events.DoString("assert(calls==0); e:setEventFilter(nil)", "filter consumed"), "filter suppresses action");
    events.ClickAt(50,50); events.RunEnterFrame();
    Check(events.DoString("assert(calls==1)", "filter cleared"), "clearing filter restores action exactly once");
    Check(events.IsWaiting(), "button action must not release the scenario wait");
    Check(events.DoString("nested={callbacks={tick=function(e) end}}", "nested callback"), "define dotted callback");
    const int stack_top = lua_gettop(events.state());
    for (int i=0; i<100; ++i) Check(events.CallGlobal("nested.callbacks.tick"), "invoke dotted callback");
    Check(lua_gettop(events.state()) == stack_top, "dotted callbacks must not leak parent tables on the Lua stack");

    // A tiny pack containing only synthetic scripts tests re-entrant native
    // dispatch: the Lua callback replaces the script being executed.
    const std::map<std::string,std::string> files = {
        {"main.iet", "*main\n[calllua function=redirect]\n[calllua function=wrong]\n[stop]\n"},
        {"next.iet", "*next\n[calllua function=right]\n"},
        {"menu.iet", "*menu\n[calllua function=right]\n[return]\n"}
    };
    std::vector<unsigned char> pack;
    auto u32=[&](uint32_t v){ for(int i=0;i<4;++i) pack.push_back((v>>(i*8))&255); };
    uint32_t index=4;
    for(const auto& f:files) index+=16+f.first.size();
    pack.insert(pack.end(),{'p','f','8'});u32(index);u32(files.size());
    uint32_t offset=7+index;
    for(const auto& f:files) {
        u32(f.first.size());pack.insert(pack.end(),f.first.begin(),f.first.end());
        u32(0);u32(offset);u32(f.second.size());offset+=f.second.size();
    }
    for(const auto& f:files) pack.insert(pack.end(),f.second.begin(),f.second.end());
    const auto path=std::filesystem::temp_directory_path()/
        ("artemis-regression-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pfs");
    { std::ofstream f(path,std::ios::binary); f.write(reinterpret_cast<const char*>(pack.data()),pack.size()); }
    artc::PackManager fixture;
    Check(fixture.OpenChain(path.string(),{0}), "open synthetic script pack");
    const auto loose=path.string()+".media";
    { std::ofstream out(loose,std::ios::binary); out<<"loose movie bytes"; }
    std::vector<uint8_t> loose_bytes;
    const auto loose_name=std::filesystem::path(loose).filename().string();
    Check(fixture.Exists(loose_name) && fixture.Read(loose_name,loose_bytes) &&
        std::string(loose_bytes.begin(),loose_bytes.end())=="loose movie bytes",
        "resource lookup falls back to loose files beside the PFS");
    Check(!fixture.Exists("../"+loose_name) && !fixture.Read(loose,loose_bytes),
        "loose resource lookup stays relative to the game directory");
    std::filesystem::remove(loose);
    artc::AsbRunner runner;runner.SetPackSource(&fixture);
    artc::LuaEngine script;
    Check(script.Init(&fixture,ini,"android",1280,720), "script engine init");
    script.SetJumpHandler([&](const std::string& f,const std::string& l){runner.Jump(f,l);});
    Check(script.DoString("value=0; function redirect(e) e:tag{'jump',file='next.iet',label='next'} end; "
                          "function right(e) value=value+1 end; function wrong(e) error('stale script') end",
                          "script functions"), "script callbacks");
    Check(runner.Jump("main.iet","main"), "load main script");
    for(int i=0;i<8 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(value==1)","new cursor"), "reentrant jump executes target exactly once");
    Check(runner.Halted(), "end of script halts without an out-of-bounds Current");
    script.SetScriptRunner(&runner);
    Check(runner.Jump("main.iet","main"), "reset original scenario");
    script.SetWaiting(true);
    const auto saved_pc = runner.CurrentIndex();
    const auto token = runner.BeginEvent(script);
    Check(!script.IsWaiting(), "event can run while scenario waits");
    runner.Jump("menu.iet","menu"); runner.EndEvent(token);
    for(int i=0;i<8 && !script.IsWaiting();++i) runner.ExecuteLine(script);
    Check(script.IsWaiting() && runner.CurrentIndex()==saved_pc,
          "menu return restores the exact interrupted cursor and wait");
    Check(script.DoString("assert(value==2); assert(#e:getScriptStack()==1)", "event return"),
          "event runs once and leaves no stale stack frame");
    runner.Halt();
    const auto idle = runner.BeginEvent(script); runner.EndEvent(idle);
    Check(runner.Halted() && script.IsWaiting(), "synchronous event preserves a halted runner");
    const auto called = runner.BeginEvent(script);
    runner.Call("menu.iet","menu"); runner.EndEvent(called);
    for(int i=0;i<8 && !script.IsWaiting();++i) runner.ExecuteLine(script);
    Check(runner.Halted() && runner.CurrentIndex()==saved_pc && script.IsWaiting(),
          "event call returns to the suspended frame, not its next instruction");
    script.SetTimedWait(30);
    const auto paused = runner.BeginEvent(script);
    script.PauseClock();
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    script.ResumeClock();
    runner.EndEvent(paused);
    Check(script.IsWaiting(), "app pause also freezes the wait suspended below a menu event");
    std::filesystem::remove(path);

    // ---- E-mote Lua surface: contract failures, then a full synthetic model ----
    Check(events.DoString("assert(e:getEmoteVersion()=='3.9.8')", "version"),
          "E-mote version string");
    Check(events.DoString("assert(e:createEmoteLayer{id='x'}==nil)", "no files"),
          "createEmoteLayer without files fails");
    Check(events.DoString("assert(e:createEmoteLayer{files={'a.psb'}}==nil)", "no id"),
          "createEmoteLayer without id fails");
    Check(events.DoString("assert(e:createEmoteLayer{id='x',files={'a.psb','b.psb'}}==nil)", "multi"),
          "multi-file archives fail explicitly");
    Check(events.DoString("assert(e:createEmoteLayer{id='x',files={'missing.psb'}}==nil)", "missing"),
          "absent model files fail");
    Check(events.DoString("assert(e:getEmoteLayer('x')==nil)", "absent"),
          "getEmoteLayer of an absent layer is nil");
    Check(lua.DoString("assert(e:createEmoteLayer{id='x',files={'missing.psb'}}==nil)", "no compositor"),
          "createEmoteLayer needs a compositor");

    // A synthetic PSB inside its own pack drives the full success path.
    const std::map<std::string,std::vector<uint8_t>> emote_files = {
        {"emote.psb", emote_fixture::EncodePsb(emote_fixture::PlayerDocument())}
    };
    std::vector<unsigned char> emote_pack;
    auto eu32=[&](uint32_t v){ for(int i=0;i<4;++i) emote_pack.push_back((v>>(i*8))&255); };
    uint32_t emote_index=4;
    for(const auto& f:emote_files) emote_index+=16+f.first.size();
    emote_pack.insert(emote_pack.end(),{'p','f','8'});eu32(emote_index);eu32(emote_files.size());
    uint32_t emote_offset=7+emote_index;
    for(const auto& f:emote_files) {
        eu32(f.first.size());emote_pack.insert(emote_pack.end(),f.first.begin(),f.first.end());
        eu32(0);eu32(emote_offset);eu32(f.second.size());emote_offset+=f.second.size();
    }
    for(const auto& f:emote_files) emote_pack.insert(emote_pack.end(),f.second.begin(),f.second.end());
    const auto emote_path=std::filesystem::temp_directory_path()/
        ("artemis-emote-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pfs");
    { std::ofstream f(emote_path,std::ios::binary); f.write(reinterpret_cast<const char*>(emote_pack.data()),emote_pack.size()); }
    artc::PackManager emote_packs;
    Check(emote_packs.OpenChain(emote_path.string(),{0}), "open synthetic E-mote pack");
    artc::Compositor emote_compositor;
    artc::LuaEngine emote;
    Check(emote.Init(&emote_packs,ini,"android",1280,720,&emote_compositor), "E-mote engine init");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='m1',files={'emote.psb'},width=1280,height=720,progress=false}
        assert(m)
        assert(e:getEmoteLayer('m1'))
        assert(m:countMainTimelines()==2)
        assert(m:getDiffTimelineLabelAt(0)=='delta')
        assert(m:countVariables()==1 and m:getVariableLabelAt(0)=='expression')
        assert(m:playTimeline('once'))
        assert(m:isTimelinePlaying('once') and not m:isTimelinePlaying('delta'))
        assert(m:getTimelineTotalFrameCount('once')==60)
        assert(m:PlayTimeline('delta',2))            -- sequential: queued behind "once"
        assert(m:countPlayingTimelines()==1)
        m:progress(1200)                             -- 72 frames: once ends, delta starts
        assert(m:isTimelinePlaying('delta'))
        assert(m:setVariable('expression',5) and m:getVariable('expression')==5)
        local ok,err=m:setVariable('missing',1);assert(ok==false and err)
        m:startWind(1,2,3)                           -- unregistered physics: stub, no error
        stale=m
    )","emote lua"),"E-mote proxy playback through the bridge");
    emote.RunEnterFrame();  // UpdateEmotes renders every live player
    Check(emote_compositor.GetLayerInfo("m1").found, "frame tick renders the E-mote layer subtree");
    Check(emote.DoString(R"(
        e:tag{'lydel',id='m1'}
        assert(e:getEmoteLayer('m1')==nil)
        local ok,err=stale:isTimelinePlaying('delta')
        assert(ok==nil and err=='E-mote layer removed')
        assert(select('#',stale:getVariable('expression'))==2)
    )","stale"),"lydel invalidates live proxies");
    std::filesystem::remove(emote_path);
    Check(script.DoString(R"(
        assert(pluto.unpersist({},"")==nil)
        local t={x=2, s="a\0b", empty={}, yes=true}
        t.self=t; t.same=t.empty; t[t.empty]="key"
        local data=pluto.persist({},t)
        assert(data:byte(1)==1)
        local copy=pluto.unpersist({},data)
        assert(copy.x==2 and copy.s=="a\0b" and copy.yes)
        assert(copy.self==copy and copy.same==copy.empty and copy[copy.empty]=="key")
        assert(pluto.unpersist({},pluto.persist({},nil))==nil)
        assert(pluto.unpersist({},pluto.persist({},false))==false)
        assert(not pcall(pluto.persist,{},function()end))
        for i=1,#data-1 do assert(not pcall(pluto.unpersist,{},data:sub(1,i))) end
        assert(not pcall(pluto.unpersist,{},data.."x"))
        local object={}; local inverse={known=object}
        assert(pluto.unpersist(inverse,pluto.persist({[object]="known"},object))==object)
        -- Hand-encoded 32-bit native Pluto string, independent of the writer.
        local s32=string.char(1,0,0,0,1,0,0,0,4,0,0,0,3,0,0,0).."a\0b"
        assert(pluto.unpersist({},s32)=="a\0b")
        assert(pluto.unpersist({},"t1 = {}\nt1[\"x\"] = 7\nreturn t1").x==7)
    )","native Pluto value codec"),"native Pluto graphs, 32-bit lengths and malformed input");
    std::cout << "audio channels and wait regressions passed\n";
}
