#include "audio/audio_channels.h"
#include "config/ini.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "script/asb_parser.h"
#include "script/expression.h"
#include "save/native_save.h"
#include "util/save_storage.h"
#include "emote_scene_fixture.h"
#include <zlib.h>
#include "render/compositor.h"
#include "render/emote_player.h"
#include "log/logger.h"
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <iostream>
#include <map>
#include <thread>
#include <vector>
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

static void EmoteProxyLifetimeRegression(artc::PackManager& packs, const artc::Ini& ini) {
    artc::Compositor compositor;
    bool finalized_after_removal=false;
    std::weak_ptr<const void> destroyed_token;
    const auto root=std::filesystem::temp_directory_path()/
        ("artemis-emote-lifetime-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root);
    {
        artc::LuaEngine engine;
        engine.SetSaveDir(root.string());
        Check(engine.Init(&packs,ini,"android",32,32,&compositor),"proxy lifetime engine init");
        Check(engine.DoString(R"(
            function assertRemoved(m)
                for _,method in ipairs{m.getVariable,m.GetVariable,m.setVariable,m.SetVariable,
                    m.setVariableDiff,m.SetVariableDiff,m.getVariableDiff,m.GetVariableDiff,
                    m.getCoord,m.GetCoord,m.contains,m.Contains,m.hitTest,m.HitTest} do
                    local ok,err=method(m) -- removed takes precedence over invalid arguments
                    assert(ok==nil and err=='E-mote layer removed')
                end
            end
            local first=e:createEmoteLayer{id='life.child',files={'emote.psb'},progress=false}
            local alias=e:getEmoteLayer{id='life.child',next=true}
            assert(first:setVariable('expression',3))
            assert(alias:GetVariable('expression')==3)
            for _,files in ipairs{{'missing.psb'},{'broken.psb'},{}, {'emote.psb','emote.psb'}} do
                assert(e:createEmoteLayer{id='life.child',files=files}==nil)
                assert(first:getVariable('expression')==3 and alias:GetVariable('expression')==3)
            end
            local fresh=e:createEmoteLayer{id='life.child',files={'emote.psb'},progress=false}
            assert(fresh:getVariable('expression')==0)
            assertRemoved(first); assertRemoved(alias)
            local current=e:getEmoteLayer('life.child')
            fresh=nil; first=nil; alias=nil; collectgarbage('collect')
            assert(current:setVariable('expression',7)) -- GC never removes the player
            assert(e:getEmoteLayer('life.child'):GetVariable('expression')==7)
            e:tag{'lydel',id='life'} -- ancestor deletion expires child instance
            local recreated=e:createEmoteLayer{id='life.child',files={'emote.psb'},progress=false}
            assertRemoved(current)
            e:tag{'lydel',id='life.child'}
            local again=e:createEmoteLayer{id='life.child',files={'emote.psb'},progress=false}
            assertRemoved(recreated)
            snapshot_old=again
            function restore_lifetime(e)
                assert(e:getEmoteLayer('life.child')==nil)
                snapshot_current=e:createEmoteLayer{id='life.child',files={'emote.psb'},progress=false}
                assertRemoved(snapshot_old)
            end
            e:setEventHandler{onLoad='restore_lifetime'}
        )","proxy instance identity"),"same-id replacement, failed creation, deletion, aliases and GC respect instance identity");
        std::vector<uint8_t> snapshot;
        Check(artc::EncodeVariableBank({},true,snapshot) &&
              artc::WriteSaveFile((root/"lifetime.dat").string(),snapshot),"write synthetic proxy lifetime checkpoint");
        Check(engine.LoadSnapshot("lifetime.dat"),"snapshot clear expires old proxy before restorer recreates same id");
        destroyed_token=engine.EmoteLifetime("life.child");
        Check(!destroyed_token.expired(),"live registry owns the instance token");
        lua_State* L=engine.state();
        lua_pushlightuserdata(L,&finalized_after_removal);
        lua_pushcclosure(L,[](lua_State* state) -> int {
            auto* result=static_cast<bool*>(lua_touserdata(state,lua_upvalueindex(1)));
            *result=lua_toboolean(state,1)!=0;
            return 0;
        },1);
        lua_setglobal(L,"mark_lifetime_finalized");
        Check(engine.DoString(R"(
            local held=snapshot_current
            function finalize_lifetime()
                local ok,err=held:GetVariable('expression')
                mark_lifetime_finalized(ok==nil and err=='E-mote layer removed')
            end
        )","proxy shutdown finalizer"),"install proxy check for engine shutdown");
        // A normal Lua userdata finalizer runs while lua_close is active,
        // after LuaEngine has cleared the registry but before engine memory dies.
        lua_newuserdata(L,1);
        lua_newtable(L);
        lua_getglobal(L,"finalize_lifetime");
        lua_setfield(L,-2,"__gc");
        lua_setmetatable(L,-2);
        lua_setglobal(L,"lifetime_finalizer");
    }
    Check(destroyed_token.expired() && finalized_after_removal,
          "engine destruction expires token before Lua finalizers and retained aliases remain safe");
    std::filesystem::remove_all(root); // only this freshly created test-owned root
}

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

    // Infinite decorative animations keep requesting redraws but are not
    // script barriers (upstream issue #7). Finite repeats/delays retain their
    // full duration, including long effects that exceed thirty seconds.
    {
        artc::Compositor animations;
        animations.Init(1280, 720);
        animations.AddTween("loop", {{"left","0,100"},{"time","1000"},{"yoyo","-1"}}, 1000);
        Check(std::isinf(animations.PendingAnimationMs(1050)),
              "infinite yoyo keeps the host redraw gate active");
        Check(animations.PendingBlockingAnimationMs(1050)==0,
              "infinite yoyo contributes no script barrier");
        animations.AddTween("finite", {{"top","0,100"},{"time","60000"},
                                     {"delay","50"},{"loop","1"}}, 1000);
        Check(animations.PendingBlockingAnimationMs(1050)==120000,
              "long finite animation retains delay and repeat duration");
        const auto revision=animations.Revision();
        Check(animations.Update(1100) && animations.Revision()>revision,
              "decorative loop still updates while the script can run");
        animations.DeleteTweens("finite");
        Check(animations.PendingBlockingAnimationMs(1100)==0 &&
              std::isinf(animations.PendingAnimationMs(1100)),
              "finishing finite work does not stop the decorative loop");
        animations.AddTween("loop2", {{"left","0,50"},{"time","250"},{"loop","-1"}}, 1100);
        Check(animations.PendingBlockingAnimationMs(1200)==0,
              "infinite loop and yoyo use the same nonblocking semantics");

        artc::LuaEngine barriers;
        Check(barriers.Init(&packs, ini, "android", 1280, 720, &animations),
              "initialize script with infinite decorative animations");
        barriers.DispatchTag("wait", {{"scenario","1"},{"input","0"}});
        Check(!barriers.IsWaiting(), "scenario wait ignores infinite decoration");
        barriers.DispatchTag("wait", {});
        Check(!barriers.IsWaiting(), "implicit wait ignores infinite decoration");
        barriers.DispatchTag("wt", {});
        Check(!barriers.IsWaiting(), "wt ignores infinite decoration");
        animations.AddTween("barrier", {{"alpha","255,0"},{"time","60000"}}, barriers.NowMs());
        barriers.DispatchTag("wait", {{"scenario","1"},{"input","0"}});
        Check(barriers.IsWaiting(), "scenario wait still blocks on a long finite tween");
        barriers.SetWaiting(false);
        barriers.DispatchTag("wt", {});
        Check(barriers.IsWaiting(), "wt still blocks on a long finite tween");
        barriers.SetWaiting(false);
        animations.DeleteTweens("barrier");
        barriers.DispatchTag("wait", {});
        Check(!barriers.IsWaiting(), "script resumes once only infinite loops remain");
    }

    // A negative repeat flag is not itself an infinite duration: zero-time
    // work executes once after its delay, and that finite delay must block.
    {
        artc::Compositor delayed;
        delayed.Init(1280, 720);
        delayed.AddTween("delayed", {{"left","0,100"},{"time","0"},
                                   {"delay","50"},{"yoyo","-1"}}, 1050);
        Check(delayed.PendingAnimationMs(1050)==50 &&
              delayed.PendingBlockingAnimationMs(1050)==50,
              "zero-duration negative-repeat segment retains its finite delay");
        Check(delayed.PendingBlockingAnimationMs(1075)==25,
              "finite delayed work retains the remaining deadline");
        Check(!delayed.Update(1075), "delayed zero-time work has not started early");
        Check(delayed.Update(1100), "delayed zero-time work executes at its deadline");
        Check(delayed.PendingAnimationMs(1100)==0 &&
              delayed.PendingBlockingAnimationMs(1100)==0,
              "zero-time negative-repeat segment finishes rather than looping");
        const auto final=delayed.GetLayerInfo("delayed");
        Check(final.found && final.left==100,
              "finite delayed segment applies its final property once");
    }
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

    Check(lua.DoString(R"(
        assert(e:var('unassigned_parameter') == '0')
        assert(e:var('t.unassigned_parameter') == '')
        e:tag{'var',name='unassigned_parameter',data=''}
        assert(e:var('unassigned_parameter') == '')
    )", "unassigned tag parameter"), "bare parameter defaults distinguish unset from explicit empty values");

    artc::Compositor compositor;
    compositor.SetProps("500.1", {{"w", "100"}, {"h", "100"}});
    const_cast<artc::Layer&>(compositor.Layers().front()).texture = 1;
    // Separate Lua instance avoids depending on game-global button tables.
    artc::LuaEngine events;
    Check(events.Init(&packs, ini, "android", 1280, 720, &compositor), "event engine init");
    Check(events.DoString(R"(
        assert(e:var('s.current_message_layer') == '')
        e:tag{'chgmsg', id='dialogue'}
        assert(e:var('s.current_message_layer') == 'dialogue')
        e:tag{'chgmsg', id='icon', layered=1}
        assert(e:var('s.current_message_layer') == 'icon')
        e:tag{'/chgmsg'}
        assert(e:var('s.current_message_layer') == 'dialogue')
        e:tag{'chgmsg', id='dialogue'}
        e:tag{'/chgmsg'}
        assert(e:var('s.current_message_layer') == 'dialogue')
        e:tag{'/chgmsg'}
        e:tag{'/chgmsg'}
        assert(e:var('s.current_message_layer') == '')
    )", "nested message selection"), "closing a nested message restores the previous selection");
    Check(events.DoString(R"(
        e:tag{'chgmsg', id='dialogue'}
        e:tag{'glyph', layer='1.90', homing='1'}
        e:tag{'print', data='AAAA'}
        e:tag{'/chgmsg'}
    )", "glyph homing"), "glyph tag homes the click-wait layer to the text end");
    const auto glyph_info = compositor.GetLayerInfo("1.90");
    // Host-mock text: 40 units per glyph on a layer created at (40, 600).
    Check(glyph_info.found && glyph_info.left == 200 && glyph_info.top == 600,
          "homing parks the glyph at the end-of-text pen across parent chains");
    Check(events.DoString("e:tag{'chgmsg', id='dialogue'}; "
        "e:tag{'glyph', layer='1.91', homing='0'}; e:tag{'print', data='AA'}; e:tag{'/chgmsg'}",
        "fixed glyph"), "homing=0 keeps the scripted glyph position");
    Check(!compositor.GetLayerInfo("1.91").found,
          "homing=0 never materializes or moves the glyph layer");
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

    // lyevent merge model: btnstat's mode-only tags must gate, never replace,
    // the handler registered for the same (id, type); the NEWEST mode tag on
    // the owner or its ancestors decides the effective state.
    auto add_hit_layer = [&](const char *lid, const char *x) {
        compositor.SetProps(lid, {{"x", x}, {"y", "0"}, {"w", "100"}, {"h", "100"}});
        for (auto &l : const_cast<std::vector<artc::Layer>&>(compositor.Layers()))
            if (l.id == lid) { l.texture = 1; break; }
    };
    add_hit_layer("500.1.0", "150");
    add_hit_layer("500.2", "300");
    add_hit_layer("500.3", "450");
    add_hit_layer("500.4", "600");
    Check(events.DoString(R"(
        qcalls=0; pushes=0
        function button_q(e,p) qcalls=qcalls+1 end
        function onkey(e,p) pushes=pushes+1 end
        -- handler first, then btnstat's disable on the same id, then a group
        -- enable on the parent (system_btnon): the newer parent mode wins.
        e:tag{'lyevent', id='500.1.0', type='click', handler='calllua',
              ['function']='button_q', lua='adv_qsave'}
        e:tag{'lyevent', id='500.1.0', type='click', mode='disable'}
        e:tag{'lyevent', id='500.1', type='click', mode='enable'}
    )", "lyevent merge setup"), "handler + mode tags registered");
    events.ClickAt(200,50); events.RunEnterFrame();
    Check(events.DoString("assert(qcalls==1)", "newest mode wins"),
          "later group enable reactivates a button disabled at creation");
    Check(events.DoString("e:tag{'lyevent', id='500.1.0', type='click', mode='disable'}",
                          "disable again"), "btnstat disable");
    events.ClickAt(200,50); events.RunEnterFrame();
    Check(events.DoString("assert(qcalls==1)", "disable gates"),
          "a newer specific disable gates the handler again");
    Check(events.DoString("assert(calls==1)", "handler kept"),
          "mode tags never replaced the sibling handler registration");
    // A layer carrying ONLY mode tags has no handler: the click falls through
    // to the plain advance path instead of firing a phantom event.
    Check(events.DoString("e:tag{'lyevent', id='500.2', type='click', mode='enable'}",
                          "mode-only layer"), "mode-only registration");
    events.SetWaiting(true);
    events.ClickAt(350,50); events.RunEnterFrame();
    Check(!events.IsWaiting(), "mode-only layer click falls back to text advance");
    // Self-contained handlers (attrs carry lua=) run their own calllua; only
    // cursor-sync handlers (no lua=) drive the CLICK key.
    Check(events.DoString(R"(
        e:tag{'setonpush', key='1', ['function']='onkey'}
        e:tag{'lyevent', id='500.3', type='click', handler='calllua',
              ['function']='button_q', lua='adv_qsave'}
        e:tag{'lyevent', id='500.4', type='click', handler='calllua',
              ['function']='button_q', key='1'}
    )", "self-contained setup"), "lua-carrying and cursor-sync buttons");
    events.ClickAt(500,50); events.RunEnterFrame();
    Check(events.DoString("assert(qcalls==2 and pushes==0)", "self-contained click"),
          "self-contained handler fires without emitting the CLICK key");
    events.ClickAt(650,50); events.RunEnterFrame();
    Check(events.DoString("assert(qcalls==3 and pushes==1)", "cursor-sync click"),
          "cursor-sync handler still drives the CLICK key exactly once");

    // Drag chain: a draggable layer's drag handler fires on move and its
    // dragout handler fires on release — the save/load screen's thumbnail
    // drag (save_thumb_drag/dragout) relies on dragout to re-enable the
    // buttons it disables at drag start.
    add_hit_layer("500.99.1", "750");
    for (auto &l : const_cast<std::vector<artc::Layer>&>(compositor.Layers()))
        if (l.id == "500.99.1") { l.draggable = true; break; }
    Check(events.DoString(R"(
        drags=0; dragouts=0
        function thumb_drag(e,p) drags=drags+1 end
        function thumb_dragout(e,p) dragouts=dragouts+1 end
        e:tag{'lyevent', id='500.99.1', type='drag', handler='calllua', ['function']='thumb_drag'}
        e:tag{'lyevent', id='500.99.1', type='dragout', handler='calllua', ['function']='thumb_dragout'}
    )", "drag chain setup"), "register drag and dragout handlers");
    events.BeginDrag(760,50);
    events.DragMove(770,50);
    events.EndDrag();
    Check(events.DoString("assert(drags==1 and dragouts==1, drags..'/'..dragouts)", "drag chain"),
          "drag move fires drag, release fires dragout exactly once each");

    // A click wait announced mid-print samples {textTween} in keyClickStart
    // (flg.waitflag); when the tween drains mid-wait the engine must
    // re-announce OUT+IN so the framework re-samples an empty reason —
    // otherwise menu buttons stay blocked for the whole wait.
    Check(events.DoString(R"(
        wins=0; wouts=0; inreason=-1
        function onwin(e)
            wins=wins+1
            local n=0; for k,v in pairs(e:getScriptWaitReason()) do n=n+1 end
            inreason=n
        end
        function onwout(e) wouts=wouts+1 end
        e:setEventHandler{onClickWaitIn='onwin', onClickWaitOut='onwout'}
    )", "wait announce handlers"), "register wait announce handlers");
    {
        // Host-mock text does not populate glyphs; inject a synthetic
        // still-running tween on the visible 500.1 layer instead.
        auto &layers = const_cast<std::vector<artc::Layer>&>(compositor.Layers());
        artc::Layer *host_layer = nullptr;
        for (auto &l : layers) if (l.id == "500.1") { host_layer = &l; break; }
        Check(host_layer != nullptr, "host layer for synthetic tween");
        host_layer->text_in.push_back({"alpha", 0, 100, 0, 0});
        artc::TextGlyph g; g.start_ms = 1e9;            // tween still running
        host_layer->glyphs.push_back(g);
    }
    events.SetWaiting(true);            // IN announced with {textTween}
    events.IsWaiting();                 // tween pending: no re-announce yet
    Check(events.DoString("assert(wins==1 and wouts==0 and inreason==1)", "mid-print IN"),
          "wait IN mid-print samples the textTween reason exactly once");
    {
        auto &layers = const_cast<std::vector<artc::Layer>&>(compositor.Layers());
        for (auto &l : layers)
            if (l.id == "500.1")
                for (auto &g : l.glyphs) g.start_ms = -1e9;  // tween drained
    }
    events.IsWaiting();                 // drained: re-announce OUT+IN
    Check(events.DoString("assert(wins==2 and wouts==1 and inreason==0)", "drained re-announce"),
          "tween draining mid-wait re-announces so the reason re-samples empty");
    events.SetWaiting(false);
    Check(events.DoString("assert(wouts==2); "
                          "e:setEventHandler{onClickWaitIn='', onClickWaitOut=''}", "wait OUT"),
          "wait end announces OUT once more");
    Check(events.DoString("nested={callbacks={tick=function(e) end}}", "nested callback"), "define dotted callback");
    const int stack_top = lua_gettop(events.state());
    for (int i=0; i<100; ++i) Check(events.CallGlobal("nested.callbacks.tick"), "invoke dotted callback");
    Check(lua_gettop(events.state()) == stack_top, "dotted callbacks must not leak parent tables on the Lua stack");

    // A tiny pack containing only synthetic scripts tests re-entrant native
    // dispatch: the Lua callback replaces the script being executed.
    const std::map<std::string,std::string> files = {
        {"main.iet", "*main\n[calllua function=redirect]\n[calllua function=wrong]\n[stop]\n"},
        {"next.iet", "*next\n[calllua function=right]\n"},
        {"menu.iet", "*menu\n[calllua function=right]\n[return]\n"},
        {"load.iet", "*load\n[calllua function=load_start file=\"$t.file\" suspend]\n[stop]\n"},
        {"dlg.iet", "*dlg\n[yesno file=\"title\"]\n[stop]\n"},
        {"park.iet", "*park\n[call file=\"ui.iet\" label=\"ui\"]\n[calllua function=dlgright]\n[stop]\n"},
        {"menu2.iet", "*menu2\n[calllua function=dlgright]\n[return]\n"},
        {"ui.iet", "*ui\n[stop]\n"},
        {"branch.iet", "*branch\n"
                       "[if estimate=\"$t.flag == 1\"]\n[calllua function=hitA]\n"
                       "[elseif estimate=\"$t.flag == 2\"]\n[calllua function=hitB]\n"
                       "[else]\n[calllua function=hitC]\n[/if]\n"
                       "[if estimate=\"$t.one == 1\"][calllua function=hitOne][/if]\n"
                       "[calllua function=hitEnd]\n[stop]\n"},
        {"macdef.iet", "*mt\n"
                       "[var name=\"t.ex\" system=\"var_exist\" target=\"time\" local=\"1\"]\n"
                       "[if estimate=\"$t.ex == 1\"][calllua function=captime data=\"$time\"][/if]\n"
                       "[return]\n"},
        {"macuse.iet", "*use\n[calllua function=seed]\n[localm]\n[mt time=\"500\"]\n"
                       "[calllua function=checkrestore]\n[mt]\n[calllua function=checkfinal]\n[stop]\n"
                       "*localm\n[calllua function=caplocal]\n[return]\n"},
        {"boot.iet", "*top\n[stop]\n*game_start\n[calllua function=hitA]\n[stop]\n"},
        {"mac2.iet", "*game_start\n[calllua function=hitB]\n[stop]\n"}
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
    runner.SetLuaEngine(&script);
    script.SetJumpHandler([&](const std::string& f,const std::string& l){runner.Jump(f,l);});
    Check(script.DoString("value=0; function redirect(e) e:tag{'jump',file='next.iet',label='next'} end; "
                          "function right(e) value=value+1 end; function wrong(e) error('stale script') end",
                          "script functions"), "script callbacks");
    Check(runner.Jump("main.iet","main"), "load main script");
    for(int i=0;i<8 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(value==1)","new cursor"), "reentrant jump executes target exactly once");
    Check(runner.Halted(), "end of script halts without an out-of-bounds Current");
    // ASB [calllua] must pass the attribute table as fn(e, param) with $vars
    // resolved — the save/load framework's load_start reads param.file and
    // load_exec reads param["0"] for the valueless `suspend` marker.
    Check(script.DoString("captured=nil; function load_start(e,param) captured=param end; "
                          "e:tag{'var',name='t.file',data='slot01.dat'}", "capture load_start"),
          "define load_start capture and seed t.file");
    Check(runner.Jump("load.iet","load"), "load calllua script");
    for(int i=0;i<4 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(captured and captured.file=='slot01.dat' and "
                          "captured['0']=='suspend')", "calllua params"),
          "calllua passes the resolved attribute table as fn(e, param)");
    // A script tag matched by neither the framework filter nor a native
    // handler falls back to the Lua global of the same name, fn(e, attrs) —
    // the dialog framework's yesno lives only as a global (its tags.yesno
    // registration is commented out in the game scripts).
    Check(script.DoString("dlgseen=nil; function yesno(e,param) dlgseen=param end",
                          "define yesno global"), "define yesno capture");
    Check(runner.Jump("dlg.iet","dlg"), "load yesno-tag script");
    for(int i=0;i<4 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(dlgseen and dlgseen.file=='title')", "global fallback"),
          "unhandled tag falls back to the Lua global of the same name");
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

    // An event jump away from a [stop]-halted frame must not preserve the
    // dead frame: the yesno dialog framework parks dialog_open at [stop],
    // jumps from the click handler to dialog_close, and expects
    // dialog_close's [return] to land at the real caller below (adv_title's
    // resume line), not back on the halted [stop].
    Check(script.DoString("dlgvalue=0; function dlgright(e) dlgvalue=dlgvalue+1 end",
                          "dialog counter"), "define dialog counter");
    Check(runner.Jump("park.iet","park"), "load park script");
    for(int i=0;i<4 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(runner.Halted(), "called ui script parks at [stop]");
    const auto dlg = runner.BeginEvent(script);
    Check(runner.Jump("menu2.iet","menu2"), "event jumps to the close script");
    runner.EndEvent(dlg);
    for(int i=0;i<8 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(dlgvalue==2)", "dialog event jump"),
          "[return] after an event jump from a halted frame lands at the real caller");
    Check(runner.Halted(), "park script then reaches its own [stop]");

    // Full ADV-framework menu round-trip: an event-driven call into a UI
    // script that parks at [stop], then the framework's explicit return tag
    // plus a trailing return-UI call must land back on the exact story
    // cursor with its click wait intact and no frames leaked.
    Check(runner.Jump("main.iet","main"), "story re-entry for menu round-trip");
    script.SetWaiting(true);
    const auto story_pc = runner.CurrentIndex();
    const auto open_ev = runner.BeginEvent(script);
    runner.Call("ui.iet","ui");
    runner.EndEvent(open_ev);
    Check(script.DoString("assert(#e:getScriptStack()==2)", "menu stack depth"),
          "menu open leaves exactly story+ui on the script stack");
    Check(!script.IsWaiting(), "ui context runs with the story wait suspended");
    for(int i=0;i<8 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(runner.Halted() && runner.CurrentFile()=="ui.iet", "ui script parks at stop");
    const auto back_ev = runner.BeginEvent(script);
    Check(runner.Return(), "framework return tag pops the story frame");
    Check(runner.CurrentFile()=="main.iet" && runner.CurrentIndex()==story_pc &&
          script.IsWaiting() && !runner.Halted(),
          "return tag lands on the story cursor with its wait restored");
    runner.Call("menu.iet","menu");   // return-UI script: runs, ends in [return]
    for(int i=0;i<8 && !script.IsWaiting();++i) runner.ExecuteLine(script);
    runner.EndEvent(back_ev);
    Check(runner.CurrentFile()=="main.iet" && runner.CurrentIndex()==story_pc &&
          script.IsWaiting() && !runner.Halted(),
          "return-ui call round-trips to the same story cursor and wait");
    Check(script.DoString("assert(#e:getScriptStack()==1)", "round-trip stack"),
          "no frames leak across the menu round-trip");

    // Text .iet conditionals: [if]/[elseif]/[else]/[/if] compile to branch
    // metadata so only the taken branch runs (they used to fall through and
    // execute every body), including the one-line `[if …]…[/if]` form the
    // 終端 macro uses for its optional time override.
    Check(script.DoString("ahits=0;bhits=0;chits=0;onehits=0;endhits=0; "
                          "function hitA(e) ahits=ahits+1 end; function hitB(e) bhits=bhits+1 end; "
                          "function hitC(e) chits=chits+1 end; function hitOne(e) onehits=onehits+1 end; "
                          "function hitEnd(e) endhits=endhits+1 end", "branch counters"),
          "define branch counters");
    Check(script.DoString("e:tag{'var',name='t.flag',data='2'}; e:tag{'var',name='t.one',data='0'}",
                          "flag=2"), "seed branch flag 2");
    Check(runner.Jump("branch.iet","branch"), "load branch script");
    for(int i=0;i<16 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(ahits==0 and bhits==1 and chits==0 and onehits==0 and endhits==1)",
                          "flag 2 branches"),
          "elseif branch runs; if/else bodies and a false one-line if are skipped");
    Check(script.DoString("ahits=0;bhits=0;chits=0;endhits=0; e:tag{'var',name='t.flag',data='9'}",
                          "flag=9"), "seed branch flag 9");
    Check(runner.Jump("branch.iet","branch"), "reload branch script");
    for(int i=0;i<16 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(ahits==0 and bhits==0 and chits==1 and endhits==1)",
                          "flag 9 branches"),
          "else branch runs when every condition is false");
    Check(script.DoString("ahits=0;chits=0;onehits=0;endhits=0; "
                          "e:tag{'var',name='t.flag',data='1'}; e:tag{'var',name='t.one',data='1'}",
                          "flag=1"), "seed branch flag 1");
    Check(runner.Jump("branch.iet","branch"), "reload branch script again");
    for(int i=0;i<16 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(ahits==1 and chits==0 and onehits==1 and endhits==1)",
                          "flag 1 branches"),
          "if branch and a true one-line if run");

    // Text .iet macros (KAG semantics): a tag matching a label calls it as a
    // subroutine — same-file directly, boot-indexed scripts (macro.iet & co.)
    // through the cross-file index. The tag's attributes seed a variable
    // scope for the macro body ([終端 time=1000] → $time), restored on
    // return; a var_exist probe inside the macro sees the attribute.
    Check(script.DoString("captured_time=nil; restored=0; final=0; localhits=0; "
                          "function captime(e,param) captured_time=param.data end; "
                          "function caplocal(e) localhits=localhits+1 end; "
                          "function seed(e) e:tag{'var',name='time',data='outer'} end; "
                          "function checkrestore(e) assert(e:var('time')=='outer','macro scope leaked') "
                          "  e:tag{'var',name='time',system='delete'} restored=1 end; "
                          "function checkfinal(e) assert(captured_time=='500','no-attr macro ran body') final=1 end",
                          "macro captures"), "define macro captures");
    Check(runner.Jump("macdef.iet","mt"), "index the macro script (boot load)");
    Check(runner.Jump("macuse.iet","use"), "load macro user script");
    for(int i=0;i<24 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(localhits==1 and captured_time=='500' and restored==1 and final==1)",
                          "macro round-trip"),
          "macros run as subroutines with attribute scopes, restored on return");
    Check(runner.Halted(), "macro user script reaches its [stop]");

    // Boot anchor: the runner stays parked on first.iet after the boot
    // interpreter ran *top, so the boot chain's bare-label jumps
    // (initLua2's `jump label=game_start`) resolve in first.iet even though
    // a macroadd'ed file defines the same label; from any other context the
    // macro registration keeps winning the cross-file index.
    Check(script.DoString("ahits=0; bhits=0", "anchor counters"), "reset anchor counters");
    Check(runner.Jump("mac2.iet","game_start"), "index the macro game_start");
    Check(runner.LoadBootAnchor("boot.iet"), "anchor the boot script");
    Check(runner.Halted() && runner.CurrentFile()=="boot.iet",
          "anchor parks the runner halted on the boot script");
    Check(runner.Jump("","game_start"), "bare boot jump");
    for(int i=0;i<4 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(ahits==1 and bhits==0)", "anchor local wins"),
          "bare label from the boot context resolves in the boot script");
    Check(runner.Jump("ui.iet","ui"), "leave the boot context");
    Check(runner.Jump("","game_start"), "bare jump away from the boot context");
    for(int i=0;i<4 && !runner.Halted();++i) runner.ExecuteLine(script);
    Check(script.DoString("assert(ahits==1 and bhits==1)", "macro index wins elsewhere"),
          "bare label away from the boot context resolves through the macro index");
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

    // A synthetic PSB inside its own pack drives the full success path. The
    // local document adds a second variable so the native difference pair
    // (SetVariableDiff) has two declared labels for ordered-pair controls.
    auto emote_document = emote_fixture::PlayerDocument();
    emote_document.root.object["metadata"].object["variableList"].array.push_back(
        emote_fixture::O({{"label", emote_fixture::S("partner")}}));
    // WP9: a shape node (type==1, 16x16 SDK unit) gives the Lua hit-test
    // surface; it sits in the base motion at model coords (20,10), scaled 2x.
    emote_document.root.object["object"].object["actor"].object["motion"].object["idle"]
        .object["layer"].array.push_back(emote_fixture::Node(1,"hit",emote_fixture::A({
            emote_fixture::Key(0,2,emote_fixture::O({{"src",emote_fixture::S("shape/rect")},
                {"coord",emote_fixture::A({emote_fixture::N(20),emote_fixture::N(10),emote_fixture::N(0)})},
                {"zx",emote_fixture::N(2)},{"zy",emote_fixture::N(2)}})),
            emote_fixture::Key(11,0)})));
    // E17's restorer owns this small selector contract; it is not a model
    // merge format or a reproduction of any game's save graph.
    auto restorer_document = emote_document;
    // A clock-only timeline keeps these safety checks independent of E3's
    // changing authored-track command semantics. Variable transitions below
    // are script-owned, not competing with a timeline expression track.
    restorer_document.root.object["metadata"].object["timelineControl"]=emote_fixture::A({
        emote_fixture::O({{"label",emote_fixture::S("safety_loop")},
            {"lastTime",emote_fixture::N(-1)},{"loopBegin",emote_fixture::N(0)},
            {"loopEnd",emote_fixture::N(20)},{"diff",emote_fixture::N(0)},
            {"variableList",emote_fixture::A({})}})});
    restorer_document.root.object["metadata"].object["selectorControl"]=emote_fixture::A({
        emote_fixture::O({{"label",emote_fixture::S("wardrobe")},
            {"enabled",emote_fixture::N(1)},
            {"optionList",emote_fixture::A({
                emote_fixture::O({{"label",emote_fixture::S("expression")},
                    {"onValue",emote_fixture::N(20)},{"offValue",emote_fixture::N(0)}}),
                emote_fixture::O({{"label",emote_fixture::S("partner")},
                    {"onValue",emote_fixture::N(1)},{"offValue",emote_fixture::N(0)}})})}})});
    const std::map<std::string,std::vector<uint8_t>> emote_files = {
        {"emote.psb", emote_fixture::EncodePsb(emote_document)},
        {"models/restorer.psb", emote_fixture::EncodePsb(restorer_document)},
        {"broken.psb", {'n','o','t','-','p','s','b'}}
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
    EmoteProxyLifetimeRegression(emote_packs,ini);
    artc::Compositor emote_compositor;
    artc::LuaEngine emote;
    Check(emote.Init(&emote_packs,ini,"android",1280,720,&emote_compositor), "E-mote engine init");
    // E7: both files really exist and decode individually. Unsupported
    // multi-file creation must fail before destroying the old same-id scene,
    // proxy target, controls, playback, or clock. Never invent merge semantics.
    Check(emote.DoString(R"(
        local single=e:createEmoteLayer{id='e7_single',files={'models/restorer.psb'},progress=false}
        assert(single and single:setVariable('wardrobe',1))
        e:tag{'lydel',id='e7_single'}
        e7=e:createEmoteLayer{id='e7',files={'models/restorer.psb'},progress=false}
        assert(e7 and e7:playTimeline('safety_loop',1))
        assert(e7:setVariable('expression',3) and e7:setVariable('partner',2))
        e7:setCoord(12,34,5,90)
        e7:progress(7)
        assert(e7:setVariable('partner',14,12,0))
        assert(e7:setTimelineBlendRatio('safety_loop',0.75))
        assert(e7:setTimelineBlendRatio('safety_loop',0.25,12,0,0))
        e7:progress(3)
        function e7_unchanged()
            for _,m in ipairs{e7,e:getEmoteLayer('e7')} do
                assert(m:getVariable('expression')==3 and m:getVariable('partner')==5)
                assert(m:countPlayingTimelines()==1 and m:isTimelinePlaying('safety_loop'))
                assert(m:getPlayingTimelineLabelAt(0)=='safety_loop' and m:getPlayingTimelineFlagsAt(0)==1)
                assert(m:getTimelineBlendRatio('safety_loop')==0.625 and m:getAnimating())
                local x,y=m:getCoord(); assert(x==12 and y==34 and m:getRot()==90)
            end
        end
        e7_unchanged()
    )","E7 live replacement setup"),"E7 seeds a live proxy and nonzero clock with pending controls");
    emote.RunEnterFrame();
    auto* const e7_player=emote.FindEmote("e7");
    const double e7_frame=e7_player->BaseFrame();
    const auto e7_revision=emote_compositor.Revision();
    const auto e7_layers=emote_compositor.Layers().size();
    Check(e7_frame==10 && emote_compositor.GetLayerInfo("e7").found,"E7 existing scene is installed");
    std::vector<std::string> e7_logs;
    artc::SetLogSink([&](int,const std::string& message){e7_logs.push_back(message);});
    const bool e7_rejected=emote.DoString(R"(
        for _,files in ipairs{{'emote.psb','models/restorer.psb'},
                              {'models/restorer.psb','emote.psb'}} do
            assert(e:createEmoteLayer{id='e7',files=files,progress=true,width=64,height=64}==nil)
            e7_unchanged()
        end
    )","E7 two real files rejected");
    artc::SetLogSink({});
    Check(e7_rejected && e7_logs==std::vector<std::string>(2,
          "createEmoteLayer: multi-file E-mote archives are not supported"),
          "E7 explicitly rejects both real-file orders rather than silently loading the first file");
    Check(emote.DoString(R"(
        assert(e:createEmoteLayer{id='e7',files={}}==nil); e7_unchanged()
        assert(e:createEmoteLayer{id='e7'}==nil); e7_unchanged()
        assert(e:createEmoteLayer{id='e7',files={'missing.psb'}}==nil); e7_unchanged()
        assert(e:createEmoteLayer{id='e7',files={'broken.psb'}}==nil); e7_unchanged()
    )","E7 zero and invalid single files"),"E7 zero/missing/invalid file boundaries preserve the existing proxy");
    Check(emote.FindEmote("e7")==e7_player && e7_player->BaseFrame()==e7_frame &&
          !e7_player->AutoProgress() && e7_player->GetZ()==5 &&
          emote_compositor.Revision()==e7_revision && emote_compositor.Layers().size()==e7_layers,
          "E7 rejected creation preserves player identity, clock, progress mode and compositor scene");
    Check(emote.DoString(R"(
        e7:progress(3)
        assert(e7:getVariable('partner')==8 and e7:getTimelineBlendRatio('safety_loop')==0.5)
        assert(e7:countPlayingTimelines()==1 and e7:isTimelinePlaying('safety_loop'))
    )","E7 pending controls survive"),"E7 failure neither cancels nor duplicates subsequent variable/blend progression");
    Check(e7_player->BaseFrame()==13,"E7 subsequent explicit progress advances the original clock once");
    Check(emote.DoString(R"(
        local replacement=e:createEmoteLayer{id='e7',files={'emote.psb'},progress=false}
        assert(replacement and replacement:getVariable('expression')==0)
        assert(replacement:getVariable('partner')==0 and replacement:countPlayingTimelines()==0)
    )","E7 valid single-file replacement"),"E7 a valid single file still replaces the same id after rejected multi-file attempts");
    Check(emote.FindEmote("e7")->BaseFrame()==0 && !emote_compositor.GetLayerInfo("e7").found,
          "E7 successful replacement resets its own clock and removes the old installed subtree");
    Check(emote.DoString("e:tag{'lydel',id='e7'}","E7 cleanup"),"remove E7 synthetic player");

    // E17 proof only: our script explicitly owns elapsed frames, current
    // values, targets and remaining linear transition durations. The existing
    // ARCV variable-bank + atomic save_storage pipeline persists that graph,
    // NOT EmotePlayer private state. One manually driven loop can be replayed
    // from start; arbitrary timeline phases/queues/blinks/RNG cannot be inferred
    // from the public getters. This is not proof that real games need no E17.
    const auto restorer_root=std::filesystem::temp_directory_path()/
        ("artemis-emote-restorer-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Check(std::filesystem::create_directory(restorer_root),"create fresh isolated E17 save root");
    const char* restorer_source=R"(
        save_calls=0; load_calls=0; frame_calls=0
        function advance_emote(n)
            model:progress(n)
            logical.elapsed=logical.elapsed+n
            logical.remaining=math.max(0,logical.remaining-n)
        end
        function save_emote(e,p)
            save_calls=save_calls+1
            assert(p.file=='synthetic.dat')
            local x,y=model:getCoord()
            local data={version=1,asset='models/restorer.psb',id='restored',
                expression=model:getVariable('expression'),selector=model:getVariable('wardrobe'),
                timeline=model:getPlayingTimelineLabelAt(0),flags=model:getPlayingTimelineFlagsAt(0),
                blend=model:getTimelineBlendRatio('safety_loop'),elapsed=logical.elapsed,
                remaining=logical.remaining,target=logical.target,blend_target=logical.blend_target,
                x=x,y=y,angle=model:getRot()}
            assert(model:countPlayingTimelines()==1)
            e:tag{'var',name='emote_checkpoint',data=pluto.persist({},data)}
        end
        function load_emote(e,p)
            load_calls=load_calls+1
            assert(p.file=='synthetic.dat' and e:getEmoteLayer('restored')==nil)
            local data=pluto.unpersist({},e:var('emote_checkpoint'))
            assert(data.version==1 and data.elapsed==3 and data.remaining==9)
            logical={elapsed=data.elapsed,remaining=data.remaining,
                target=data.target,blend_target=data.blend_target}
            model=assert(e:createEmoteLayer{id=data.id,files={data.asset},progress=false})
            assert(model:playTimeline(data.timeline,data.flags))
            model:progress(data.elapsed) -- replay our explicit single-loop history
            assert(model:setVariable('expression',data.expression))
            assert(model:setVariable('wardrobe',data.selector))
            assert(model:setTimelineBlendRatio(data.timeline,data.blend))
            model:setCoord(data.x,data.y,0,data.angle)
            assert(model:setVariable('expression',data.target,data.remaining,0))
            assert(model:setTimelineBlendRatio(data.timeline,data.blend_target,data.remaining,0,0))
        end
        function tick_emote(e)
            frame_calls=frame_calls+1
            advance_emote(1) -- progress=false: exactly one script-owned step
        end
        e:setEventHandler{onSave='save_emote',onLoad='load_emote'}
    )";
    {
        artc::Compositor saving_compositor;
        artc::LuaEngine saving;
        saving.SetSaveDir(restorer_root.string());
        Check(saving.Init(&emote_packs,ini,"android",32,32,&saving_compositor),"E17 saver init in new test root");
        Check(saving.DoString(restorer_source,"E17 original restorer script") && saving.DoString(R"(
            logical={elapsed=0,remaining=12,target=14,blend_target=0.25}
            model=assert(e:createEmoteLayer{id='restored',files={'models/restorer.psb'},progress=false})
            assert(model:playTimeline('safety_loop',1))
            model:setCoord(17,23,0,45)
            assert(model:setVariable('wardrobe',0.25) and model:setVariable('expression',2))
            assert(model:setVariable('expression',14,12,0))
            assert(model:setTimelineBlendRatio('safety_loop',0.75))
            assert(model:setTimelineBlendRatio('safety_loop',0.25,12,0,0))
            advance_emote(3)
            assert(model:getVariable('expression')==5 and model:getTimelineBlendRatio('safety_loop')==0.625)
        )","E17 logical scene"),"E17 prepares only self-authored script state");
        Check(saving.SaveSnapshot("synthetic.dat"),"E17 commits checkpoint through onSave and atomic ARCV pipeline");
        Check(saving.DoString("assert(save_calls==1 and load_calls==0)","E17 save callback count"),"E17 onSave runs once");
    } // The original player, Lua VM and compositor are gone before restoration.
    std::vector<uint8_t> checkpoint;
    artc::VariableBank checkpoint_bank;
    Check(artc::ReadSaveFile((restorer_root/"synthetic.dat").string(),checkpoint) &&
          checkpoint.size()>4 && std::string(checkpoint.begin(),checkpoint.begin()+4)=="ARCV" &&
          artc::DecodeVariableBank(checkpoint,true,checkpoint_bank) && checkpoint_bank.size()==1 &&
          checkpoint_bank.count("emote_checkpoint")==1,
          "E17 slot is our ARCV graph, not a private-player serializer or native BOWS export");
    {
        artc::Compositor restored_compositor;
        artc::LuaEngine restored;
        restored.SetSaveDir(restorer_root.string());
        Check(restored.Init(&emote_packs,ini,"android",32,32,&restored_compositor),"E17 cold restorer init");
        Check(restored.DoString(restorer_source,"E17 cold callback install") && restored.DoString(R"(
            assert(model==nil and logical==nil and e:getEmoteLayer('restored')==nil)
            e:tag{'var',name='emote_checkpoint',data='replace this system-bank copy from the slot'}
        )","E17 no player before load"),"E17 fresh VM has no implicitly restored player");
        Check(restored.LoadSnapshot("synthetic.dat"),"E17 restores the saved graph through existing onLoad pipeline");
        Check(restored.DoString(R"(
            assert(load_calls==1 and save_calls==0 and frame_calls==0)
            assert(model:getVariable('expression')==5 and model:getVariable('wardrobe')==0.25)
            assert(model:isTimelinePlaying('safety_loop') and model:countPlayingTimelines()==1)
            assert(model:getPlayingTimelineLabelAt(0)=='safety_loop' and model:getPlayingTimelineFlagsAt(0)==1)
            assert(model:getTimelineBlendRatio('safety_loop')==0.625)
            local x,y=model:getCoord(); assert(x==17 and y==23 and model:getRot()==45)
        )","E17 restored public state"),"E17 expression, selector, timeline, blend and placement match explicit saved fields");
        Check(restored.FindEmote("restored")->BaseFrame()==3 && !restored.FindEmote("restored")->AutoProgress(),
              "E17 script replays its explicit elapsed history without enabling wall-clock progress");
        auto restorer_picture_width=[&]() {
            int sum=0;
            for(const auto& layer:restored_compositor.Layers())
                if(layer.id.rfind("restored.",0)==0 && layer.id.size()>=7 &&
                   layer.id.compare(layer.id.size()-7,7,".000000")==0)
                    sum+=int(restored_compositor.GetLayerInfo(layer.id).width);
            return sum;
        };
        restored.RunEnterFrame(); // render only; no tick handler installed yet
        Check(restorer_picture_width()==8 && restored.FindEmote("restored")->BaseFrame()==3,
              "E17 restored selector chooses the wide synthetic face without an implicit first-frame advance");
        Check(restored.DoString("assert(model:setVariable('wardrobe',1))","E17 alternate selector"),"change restored selector");
        restored.RunEnterFrame();
        Check(restorer_picture_width()==6,"E17 restored selector actually affects the rendered scene, not only its getter");
        Check(restored.DoString(R"(
            assert(model:setVariable('wardrobe',0.25))
            e:setEventHandler{onEnterFrame='tick_emote'}
        )","E17 script-owned frame driver"),"install one explicit frame driver after reconstruction");
        restored.RunEnterFrame();
        Check(restored.DoString(R"(
            assert(load_calls==1 and frame_calls==1 and logical.elapsed==4 and logical.remaining==8)
            assert(math.abs(model:getVariable('expression')-6)<1e-9)
            assert(math.abs(model:getTimelineBlendRatio('safety_loop')-(0.625-0.375/9))<1e-9)
            assert(model:countPlayingTimelines()==1 and model:isTimelinePlaying('safety_loop'))
        )","E17 first driven frame"),"E17 first frame advances rearmed controls once, without duplicate playback/restoration");
        Check(restored.FindEmote("restored")->BaseFrame()==4 && restorer_picture_width()==8,
              "E17 first driven frame advances the clock once and keeps restored selector composition");
    }
    std::filesystem::remove_all(restorer_root); // only this freshly created test-owned root
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='blend_flags',files={'emote.psb'},progress=false}
        assert(m and m:playTimeline('loop'))
        assert(m:setTimelineBlendRatio('loop',0,0,0,0))
        m:progress(0)
        assert(m:isTimelinePlaying('loop'), 'numeric zero must not arm fade-out removal')
        e:tag{'lydel',id='blend_flags'}
    )","blend numeric zero"),"numeric zero blend flag retains zero-weight playback");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='blend_errors',files={'emote.psb'},progress=false}
        assert(m and m:playTimeline('loop'))
        assert(m:setTimelineBlendRatio('loop',0.75,0,0,false))
        for _,method in ipairs{m.setTimelineBlendRatio,m.SetTimelineBlendRatio} do
            for _,args in ipairs{
                {{},0,0,0,false},             -- label
                {'loop',nil,0,0,false},        -- required ratio
                {'loop',0,{},0,false},         -- optional frames
                {'loop',0,0,{},false},         -- optional ease
                {'loop',nil,{},{},false}       -- multiple old positional errors
            } do
                local old_ok,old_err=pcall(method,m,unpack(args,1,5))
                assert(old_ok==false and type(old_err)=='string')
                assert(old_err:find('expected',1,true))
                for _,bad in ipairs{{},'0',0/0,math.huge} do
                    args[5]=bad
                    local ok,err=pcall(method,m,unpack(args,1,5))
                    assert(ok==false, 'positional errors must throw before flags validation')
                    assert(err==old_err, 'invalid flags must preserve the positional error')
                    assert(m:getTimelineBlendRatio('loop')==0.75)
                    m:progress(0)
                    assert(m:isTimelinePlaying('loop'))
                end
            end
        end
        e:tag{'lydel',id='blend_errors'}
    )","blend positional error precedence"),"blend positional errors retain pcall failure and error shape with invalid flags");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='blend_flags',files={'emote.psb'},progress=false}
        local function blend(flag, remove, frames)
            assert(m:playTimeline('loop'))
            assert(m:SetTimelineBlendRatio('loop',0,frames,0,flag))
            if frames > 0 then
                m:progress(frames/2)
                assert(m:isTimelinePlaying('loop'))
                assert(math.abs(m:getTimelineBlendRatio('loop')-0.5)<1e-9)
                m:progress(frames/2)
            else
                m:progress(0)
            end
            assert(m:isTimelinePlaying('loop') == not remove)
        end
        for _,frames in ipairs{0,4} do
            blend(0,false,frames)
            blend(false,false,frames)
            blend(1,true,frames)
            blend(true,true,frames)
            -- Existing C++ contract: any finite nonzero value, not bit0 only.
            blend(2,true,frames)
            blend(-2,true,frames)
            blend(0.5,true,frames)
        end
        assert(m:playTimeline('loop'))
        assert(m:setTimelineBlendRatio('loop',0))    -- omitted flags
        m:progress(0)
        assert(m:isTimelinePlaying('loop'))
        assert(m:fadeOutTimeline('loop',4,0))
        assert(m:setTimelineBlendRatio('loop',0,0,0,0)) -- cancel pending auto-stop
        m:progress(4)
        assert(m:isTimelinePlaying('loop'))
        for _,bad in ipairs{'0','1','',{},function()end,0/0,math.huge,-math.huge} do
            assert(m:setTimelineBlendRatio('loop',0.75,0,0,false))
            local ok,err=m:setTimelineBlendRatio('loop',0,0,0,bad)
            assert(ok==false and err:find('flags',1,true))
            assert(m:getTimelineBlendRatio('loop')==0.75)
            m:progress(0)
            assert(m:isTimelinePlaying('loop'))
        end
        e:tag{'lydel',id='blend_flags'}
    )","blend flags contract"),"blend flags numeric/boolean, immediate/timed, cancellation and invalid inputs");

    // E3 normal-path evidence (docs/emote-driver-contract.md): bit0 controls
    // stop-all, bit1 selects private noninstant tracks, never a timeline FIFO.
    // These synthetic Lua checks do not establish native ABI/visual equivalence.
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='local_e3',files={'emote.psb'},progress=false}
        for _,flags in ipairs{0,1,2,3} do
            m:stopTimeline('')
            assert(m:playTimeline('once',1) and m:playTimeline('loop',3))
            assert(m:PlayTimeline('delta',flags))
            local parallel=flags%2==1
            assert(m:countPlayingTimelines()==(parallel and 3 or 1))
            assert(m:isTimelinePlaying('once')==parallel and m:isTimelinePlaying('loop')==parallel)
            assert(m:isTimelinePlaying('delta')) -- bit1 starts immediately, not after once
            local index=parallel and 2 or 0
            assert(m:getPlayingTimelineLabelAt(index)=='delta' and m:getPlayingTimelineFlagsAt(index)==flags)
            assert(m:playTimeline('delta',flags))
            assert(m:countPlayingTimelines()==(parallel and 3 or 1))
            assert(m:getPlayingTimelineLabelAt(index)=='delta')
        end
        assert(m:playTimeline('once')) -- omitted flags are 0, not parallel
        assert(m:countPlayingTimelines()==1 and m:getPlayingTimelineFlagsAt(0)==0)
        assert(not m:isTimelinePlaying('loop') and not m:isTimelinePlaying('delta'))
        assert(m:playTimeline('delta',2))
        assert(m:countPlayingTimelines()==1 and not m:isTimelinePlaying('once'))
        assert(m:setVariable('expression',7))
        m:progress(5)
        assert(m:getVariable('expression')==7) -- private diff never replaces base getter
        local ok,err=m:playTimeline('missing',0)
        assert(ok==false and err and m:isTimelinePlaying('delta'))
        for _,bad in ipairs{0.5,0/0,math.huge,-math.huge,2147483648} do
            ok,err=m:PlayTimeline('once',bad)
            assert(ok==false and err and m:countPlayingTimelines()==1)
            assert(m:getVariable('expression')==7 and m:getPlayingTimelineFlagsAt(0)==2)
        end
        assert(m:playTimeline('loop',1)) -- ordinary track commands base, irrespective of metadata.diff
        m:progress(5)
        assert(math.abs(m:getVariable('expression')-5)<1e-9)
        m:stopTimeline('loop')
        assert(m:isTimelinePlaying('delta') and m:countPlayingTimelines()==1)
        m:StopTimeline('')
        assert(not m:isTimelinePlaying('') and m:countPlayingTimelines()==0)
        e:tag{'lydel',id='local_e3'}
    )","E3 flags and private tracks"),"Lua flags0/2 stop-all, flags1/3 parallel, immediate private playback and atomic rejection");
    Check(emote.DoString(R"(
        e3=e:createEmoteLayer{id='e3_clock',files={'emote.psb'},progress=false}
        local m=e3
        assert(m:playTimeline('once',1) and m:playTimeline('delta',3))
        m:progress(5)
        m:setQueuing(true)
        assert(m:setVariable('partner',10,60,0))
        assert(m:setVariable('partner',70,60,0))
        assert(m:setVariableDiff('expression','partner',6,60,0))
        assert(m:setVariableDiff('expression','partner',-4,60,0))
        assert(m:setTimelineBlendRatio('delta',0.5,60,0,0))
        assert(m:setTimelineBlendRatio('delta',0.25,60,0,0))
        m:Step()
        assert(not m:isTimelinePlaying('once') and m:isTimelinePlaying('delta'))
        assert(m:getVariable('expression')==60 and m:getVariable('partner')==70)
        assert(m:getVariableDiff('expression','partner')==-4 and m:getTimelineBlendRatio('delta')==0.25)
        m:step() -- repeat is not another Progress(1)
        assert(m:playTimeline('once',1))
        m:Skip()
        assert(not m:isTimelinePlaying('once') and m:isTimelinePlaying('delta'))
        assert(m:getVariable('expression')==60 and m:countPlayingTimelines()==1)
    )","E3 Step and Skip"),"Step/Skip remove finite playback, retain loops and finish final queued global targets");
    Check(emote.FindEmote("e3_clock")->BaseFrame()==5,
          "Lua Step, repeated Step and Skip never advance the base/scene clock");
    Check(emote.DoString(R"(
        local m=e3
        m:stopTimeline(''); m:setQueuing(false)
        assert(m:setVariable('partner',0) and m:setVariableDiff('expression','partner',0))
        assert(m:playTimeline('once',1) and m:playTimeline('delta',3))
        m:progress(5)
        assert(m:setVariable('partner',60,60,0))
        assert(m:setVariableDiff('expression','partner',12,60,0))
        assert(m:setTimelineBlendRatio('delta',0.25,60,0,0))
        m:Pass()
        assert(not m:isTimelinePlaying('once') and m:isTimelinePlaying('delta'))
        assert(math.abs(m:getVariable('expression')-5)<1e-9)
        assert(m:getVariable('partner')==0 and m:getVariableDiff('expression','partner')==0)
        assert(m:getTimelineBlendRatio('delta')==1) -- loop and its pending blend untouched
    )","E3 ordinary Pass"),"Pass removes ordinary nonloop but submits rather than finishes remaining commands");
    Check(emote.FindEmote("e3_clock")->BaseFrame()==10,"ordinary Pass does not advance base frame");
    const bool e3_private_pass=emote.DoString(R"(
        local m=e3
        m:progress(10)
        assert(math.abs(m:getVariable('expression')-(5+55/6))<1e-9) -- Pass uses remaining record.time=60
        assert(math.abs(m:getVariable('partner')-10)<1e-9)
        assert(math.abs(m:getVariableDiff('expression','partner')-2)<1e-9)
        assert(math.abs(m:getTimelineBlendRatio('delta')-0.875)<1e-9)
        assert(m:playTimeline('once',3))
        m:Pass()
        assert(m:isTimelinePlaying('once') and m:getTimelineBlendRatio('once')==1)
        m:progress(10)
        assert(m:isTimelinePlaying('once') and math.abs(m:getTimelineBlendRatio('once')-0.5)<1e-9)
        m:pass() -- repeat guard must not restart the 20-frame private fade
        m:progress(9)
        assert(m:isTimelinePlaying('once') and math.abs(m:getTimelineBlendRatio('once')-0.05)<1e-9)
        m:progress(1)
        assert(math.abs(m:getVariable('partner')-30)<1e-9)
        assert(math.abs(m:getVariableDiff('expression','partner')-6)<1e-9)
        assert(not m:isTimelinePlaying('once') and m:isTimelinePlaying('delta'),
            'private fade must remove at 20 frames: playing='..tostring(m:isTimelinePlaying('once'))..
            ' blend='..string.format('%.17g',m:getTimelineBlendRatio('once')))
    )","E3 private Pass fade");
    // Defer this result so a boundary failure still allows the E5/E2/bridge
    // safety nets below to run; never relax the exact 20-frame removal contract.
    Check(emote.DoString("e:tag{'lydel',id='e3_clock'}","E3 clock cleanup"),"remove E3 clock fixture");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='e3_commands',files={'emote.psb'},progress=false}
        assert(m:getQueuing()==false and m:GetQueuing()==false)
        m:SetQueuing(true)
        for _,method in ipairs{m.setQueuing,m.SetQueuing} do
            for _,bad in ipairs{0,1,'true',{},function()end} do
                local ok,err=method(m,bad)
                assert(ok==false and err and m:getQueuing()==true)
            end
        end
        assert(m:playTimeline('loop',3) and m:playTimeline('delta',3))
        assert(m:countPlayingTimelines()==2) -- queuing=true still does not serialize timelines
        m:stopTimeline('delta')
        assert(m:setVariable('partner',10,12,0) and m:setVariable('partner',-2,12,0))
        assert(m:setVariableDiff('expression','partner',8,12,0))
        assert(m:setVariableDiff('expression','partner',-4,12,0))
        assert(m:setTimelineBlendRatio('loop',0.5,12,0,0))
        assert(m:setTimelineBlendRatio('loop',0.25,12,0,0))
        m:Progress(18)
        assert(math.abs(m:getVariable('partner')-4)<1e-9)
        assert(math.abs(m:getVariableDiff('expression','partner')-2)<1e-9)
        assert(math.abs(m:getTimelineBlendRatio('loop')-0.375)<1e-9)
        m:Pass()
        assert(math.abs(m:getVariable('partner')-4)<1e-9)
        assert(math.abs(m:getVariableDiff('expression','partner')-2)<1e-9)
        m:Step()
        assert(m:getVariable('partner')==-2 and m:getVariableDiff('expression','partner')==-4)
        assert(m:getTimelineBlendRatio('loop')==0.25)
        m:stopTimeline('')
        assert(m:setVariable('partner',10,12,0) and m:setVariable('partner',20,12,0))
        assert(m:setVariable('partner',-3)) -- immediate command clears active and queued commands
        m:Step(); assert(m:getVariable('partner')==-3 and not m:isAnimating())
        assert(m:setVariable('partner',10,12,0) and m:setVariable('partner',20,12,0))
        m:setQueuing(false)
        assert(m:setVariable('partner',6,12,0))
        m:progress(6)
        assert(math.abs(m:getVariable('partner')-1.5)<1e-9)
        m:Skip(); assert(m:getVariable('partner')==6 and not m:isAnimating())
        e:tag{'lydel',id='e3_commands'}
    )","E3 independent command queues"),"Lua Queuing aliases, validation, per-channel elapsed carry, final target and immediate/replacement clearing");

    Check(emote.DoString(R"(
        physics=e:createEmoteLayer{id='physics',files={'emote.psb'},progress=false}
        physics:setCoord(12,34,0,90); physics:setScale(2,3,0,0)
        physics:setHairScale(0.5); physics:setBustScale(0.75)
        assert(physics:playTimeline('once'))
        assert(physics:playTimeline('delta',3))
        assert(physics:setVariable('partner',10,60,0))
        assert(physics:setTimelineBlendRatio('once',0.25,60,0,0))
    )","physics setup"),"physics validation fixture has active controls and concurrent private playback");
    const double physics_frame = emote.FindEmote("physics")->BaseFrame();
    std::vector<std::string> physics_logs;
    artc::SetLogSink([&](int, const std::string& message) { physics_logs.push_back(message); });
    const bool physics_valid = emote.DoString(R"(
        local m=physics
        local function unsupported(method,...)
            local result={method(m,...)}
            assert(#result==2 and result[1]==false)
            assert(result[2]:find('unsupported E-mote physics:',1,true))
        end
        for i=1,3 do
            for _,part in ipairs{'bust','hair','parts'} do
                unsupported(m.setOuterForce,part,1,-2,0,0.5)
                unsupported(m.SetOuterForce,part,1,-2,0,0.5)
            end
            unsupported(m.setOuterRot,1,-2,0.5)
            unsupported(m.SetOuterRot,1,-2,0.5)
        end
    )","physics unsupported");
    artc::SetLogSink({});
    Check(physics_valid,"valid outer physics methods and aliases explicitly return false + unsupported reason");
    Check(physics_logs.size()==2 &&
          physics_logs[0]=="emote: unsupported E-mote physics: setOuterForce" &&
          physics_logs[1]=="emote: unsupported E-mote physics: setOuterRot",
          "valid unsupported physics logs once per canonical method, not once per call/part/alias");
    Check(emote.DoString(R"(
        local m=physics
        local function invalid(method,...)
            local result={method(m,...)}
            assert(#result==2 and result[1]==false and type(result[2])=='string')
            assert(not result[2]:find('unsupported',1,true))
        end
        for _,method in ipairs{m.setOuterForce,m.SetOuterForce} do
            invalid(method); invalid(method,'bust',1,2,3)
            invalid(method,'bust',1,2,3,4,5)
            for _,part in ipairs{'','head','Bust','bust\0junk',1,true,{}} do
                invalid(method,part,1,2,3,4)
            end
            invalid(method,nil,1,2,3,4)
            for i=1,4 do
                for _,bad in ipairs{'1',true,{},function()end,0/0,math.huge,-math.huge} do
                    local args={1,2,3,4}; args[i]=bad
                    invalid(method,'hair',unpack(args))
                end
                local args={1,2,3,4}; args[i]=nil
                invalid(method,'parts',unpack(args,1,4))
            end
        end
        for _,method in ipairs{m.setOuterRot,m.SetOuterRot} do
            invalid(method); invalid(method,1,2); invalid(method,1,2,3,4)
            for i=1,3 do
                for _,bad in ipairs{'1',false,{},function()end,0/0,math.huge,-math.huge} do
                    local args={1,2,3}; args[i]=bad
                    invalid(method,unpack(args))
                end
                local args={1,2,3}; args[i]=nil
                invalid(method,unpack(args,1,3))
            end
        end
        local x,y=m:getCoord(); assert(x==12 and y==34 and m:getRot()==90)
        local sx,sy=m:getScale(); assert(sx==2 and sy==3)
        assert(m:getVariable('partner')==0 and m:getTimelineBlendRatio('once')==1)
        assert(m:isTimelinePlaying('once') and m:isTimelinePlaying('delta'))
        assert(m:countPlayingTimelines()==2 and m:getPlayingTimelineFlagsAt(1)==3 and m:getAnimating())
    )","physics malformed calls"),"physics rejects arity, part, coercible strings, types and nonfinite values without mutation");
    Check(emote.FindEmote("physics")->BaseFrame()==physics_frame &&
          emote.FindEmote("physics")->HairScale()==0.5 && emote.FindEmote("physics")->BustScale()==0.75,
          "outer physics leaves clock and stored physical scales unchanged");
    physics_logs.clear();
    artc::SetLogSink([&](int, const std::string& message) { physics_logs.push_back(message); });
    const bool discovery = emote.DoString(R"(
        for i=1,2 do
            assert(select('#',physics:startWind())==0)
            assert(select('#',physics:unknownE5Probe(1,{},'x'))==0)
            -- E15 frozen: both names must remain discovery stubs, not a
            -- registered SkipToSync implementation with invented semantics.
            assert(select('#',physics:skipToSync())==0)
            assert(select('#',physics:SkipToSync())==0)
        end
    )","unknown physics and frozen sync");
    artc::SetLogSink({});
    Check(discovery && physics_logs==std::vector<std::string>{
              "UNIMPLEMENTED: emote:startWind", "UNIMPLEMENTED: emote:unknownE5Probe",
              "UNIMPLEMENTED: emote:skipToSync", "UNIMPLEMENTED: emote:SkipToSync"},
          "unknown methods and unknown-signature startWind preserve once-only discovery; E15 remains unregistered");
    Check(emote.FindEmote("physics")->BaseFrame()==physics_frame,
          "frozen Lua skipToSync stub does not move the base clock");
    Check(emote.DoString(R"(
        physics:progress(30)
        assert(math.abs(physics:getVariable('partner')-5)<1e-9)
        assert(math.abs(physics:getTimelineBlendRatio('once')-0.625)<1e-9)
        physics:progress(30)
        assert(not physics:isTimelinePlaying('once') and physics:isTimelinePlaying('delta'))
        assert(physics:countPlayingTimelines()==1) -- finite end never releases/restarts a FIFO entry
        e:tag{'lydel',id='physics'}
        for _,method in ipairs{physics.setOuterForce,physics.SetOuterForce,
                               physics.setOuterRot,physics.SetOuterRot,
                               physics.setTimelineBlendRatio} do
            -- No arguments: stale resolution must run before arity/type checks.
            local ok,err=method(physics)
            assert(ok==nil and err=='E-mote layer removed')
        end
    )","physics stale precedence"),"physics preserves pending controls/concurrent playback and resolves stale handles before validation");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='pair_controls',files={'emote.psb'},progress=false}
        assert(m)
        assert(m:getVariableDiff('expression','partner')==0)
        assert(m:GetVariableDiff('partner','expression')==0)
        assert(m:getVariableDiff('missing','partner')==0)
        assert(m:setVariable('expression',3) and m:SetVariable('partner',2))
        assert(m:SetVariableDiff('expression','partner',8,60,1))
        assert(m:GetVariableDiff('expression','partner')==0 and m:isAnimating())
        m:Progress(30)
        assert(math.abs(m:getVariableDiff('expression','partner')-2)<1e-10)
        assert(m:getVariable('expression')==3 and m:GetVariable('partner')==2)
        assert(m:setVariableDiff('partner','expression',-5))
        assert(m:GetVariableDiff('partner','expression')==-5)
        assert(m:getVariableDiff('expression','partner')==2)
        assert(m:setVariableDiff('partner','partner',-1))
        assert(m:getVariableDiff('partner','partner')==-1)
        for _,bad in ipairs{0/0,math.huge,-math.huge} do
            for _,args in ipairs{{bad,60,0},{7,bad,0},{7,60,bad}} do
                local ok,err=m:setVariableDiff('expression','partner',unpack(args))
                assert(ok==false and type(err)=='string')
                assert(m:getVariableDiff('expression','partner')==2)
            end
            local ok,err=m:SetVariableDiff('expression','expression',bad,60,0)
            assert(ok==false and err and m:GetVariableDiff('expression','expression')==0)
        end
        for _,args in ipairs{{'missing','partner'},{'expression','missing'}} do
            local ok,err=m:setVariableDiff(args[1],args[2],1)
            assert(ok==false and err)
        end
        m:progress(15)
        assert(math.abs(m:getVariableDiff('expression','partner')-4.5)<1e-10)
        assert(m:setVariableDiff('expression','partner',-3.5,60,-1))
        m:progress(15)
        assert(math.abs(m:GetVariableDiff('expression','partner')-0.5)<1e-10)
        m:Pass()
        assert(math.abs(m:getVariableDiff('expression','partner')-0.5)<1e-10 and m:getAnimating())
        m:Step() -- only Step/Skip finish the global pair control, not Pass
        assert(m:getVariableDiff('expression','partner')==-3.5 and not m:getAnimating())
        assert(m:SetVariableDiff('expression','partner',6,60))
        m:skip()
        assert(m:GetVariableDiff('expression','partner')==6 and not m:isAnimating())
        e:tag{'lydel',id='pair_controls'}
        for _,method in ipairs{m.setVariableDiff,m.SetVariableDiff,m.getVariableDiff,m.GetVariableDiff} do
            local ok,err=method(m) -- stale lookup precedes malformed arguments
            assert(ok==nil and err=='E-mote layer removed')
        end
        local fresh=e:createEmoteLayer{id='pair_controls',files={'emote.psb'},progress=false}
        assert(fresh:getVariableDiff('expression','partner')==0 and fresh:getVariable('partner')==0)
        for _,method in ipairs{m.setVariableDiff,m.SetVariableDiff,m.getVariableDiff,m.GetVariableDiff} do
            local ok,err=method(m) -- same id must never resurrect a removed instance
            assert(ok==nil and err=='E-mote layer removed')
        end
        e:tag{'lydel',id='pair_controls'}
    )","ordered pair lua"),"Lua ordered-pair aliases, frame/ease units, validation and stale lifetime");
    Check(emote.DoString(R"(
        local m=e:createEmoteLayer{id='m1',files={'emote.psb'},width=1280,height=720,progress=false}
        assert(m)
        assert(e:getEmoteLayer('m1'))
        assert(m:countMainTimelines()==2)
        assert(m:getDiffTimelineLabelAt(0)=='delta')
        assert(m:countVariables()==2 and m:getVariableLabelAt(0)=='expression' and m:getVariableLabelAt(1)=='partner')
        assert(m:playTimeline('once'))
        assert(m:isTimelinePlaying('once') and not m:isTimelinePlaying('delta'))
        assert(m:getTimelineTotalFrameCount('once')==60)
        assert(m:PlayTimeline('delta',2))            -- private diff, bit0 clear removes "once"
        assert(m:countPlayingTimelines()==1 and not m:isTimelinePlaying('once'))
        m:progress(12)                              -- Lua progress accepts 60fps frame counts
        -- Stop-all removes playback, not an ordinary setter command already
        -- submitted by once; delta's private track does not overwrite that base.
        assert(m:isTimelinePlaying('delta') and math.abs(m:getVariable('expression')-12)<1e-9)
        assert(m:setVariable('expression',5) and m:getVariable('expression')==5)
        local ok,err=m:setVariable('missing',1);assert(ok==false and err)
        m:setScale(1.5,0,0)                          -- Artemis: (scale, origin_x, origin_y)
        local sx,sy=m:getScale();assert(sx==1.5 and sy==1.5)
        m:setScale(2,3,0,0)                          -- per-axis extension
        sx,sy=m:getScale();assert(sx==2 and sy==3)
        m:setCoord(12,34,0,90)                       -- Artemis: (x, y, z, angle)
        local cx,cy=m:getCoord();assert(cx==12 and cy==34 and m:getRot()==90)
        m:setGrayscale(0.5,0,0)
        m:setMeshDivisionRatio(0.8)                  -- recorded knobs, no error
        m:setHairScale(0.5)
        m:setBustScale(0.5)
        assert(m:setVariableDiff('expression','partner',4,0,0))
        assert(m:getVariable('expression')==5 and m:getVariable('partner')==0)
        assert(m:getVariableDiff('expression','partner')==4)
        local dok,derr=m:setVariableDiff('expression','missing',1,0,0);assert(dok==false and derr)
        -- frame units: 60 frames = 1 s; after 30 frames the transition is half done
        m:setVariable('expression',0,0,0)
        assert(m:setVariable('expression',10,60,0))
        m:progress(30)
        assert(math.abs(m:getVariable('expression')-5)<1e-9)
        assert(m:fadeInTimeline('delta',150,0))      -- fade frames + ease float
        assert(m:fadeOutTimeline('delta',150,0))
        assert(m:setTimelineBlendRatio('delta',0.5,100,0))
        m:step()                                    -- cross loop/control boundary, not Progress(1)
        assert(e:getEmoteLayer{id='m1',next=true})   -- table form + next
        m:startWind(1,2,3)                           -- unregistered physics: stub, no error
        stale=m
    )","emote lua"),"E-mote proxy playback through the bridge");
    emote.RunEnterFrame();  // UpdateEmotes renders every live player
    Check(emote_compositor.GetLayerInfo("m1").found, "frame tick renders the E-mote layer subtree");
    Check(emote.DoString(R"(
        -- WP9: shape hit tests run against the rendered transforms. The layer
        -- was rendered with scale (2,3) + rotation 90 and coord (12,34) inside
        -- the 1280x720 createEmoteLayer box, so the shape at model (20,10)
        -- lands at box-relative (-18,74) and world (622,434).
        local m1p=e:getEmoteLayer('m1')
        assert(m1p:contains('hit',622,434))
        assert(not m1p:contains('hit',0,0))
        assert(not m1p:contains('missing',622,434))
        assert(m1p:hitTest(622,434))
        e:tag{'lydel',id='m1'}
        assert(e:getEmoteLayer('m1')==nil)
        local ok,err=stale:isTimelinePlaying('delta')
        assert(ok==nil and err=='E-mote layer removed')
        assert(select('#',stale:getVariable('expression'))==2)
    )","hit+stale"),"lydel invalidates live proxies");
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
    // Text .iet tag lines keep their meaning when a `//` comment (or stray
    // tab) trails the closing bracket — system/system.iet adv_save ends
    // branches with `[ui_return]	// uiに戻る`; classifying that as scenario
    // text drops the tag and the framework's button lock never releases.
    {
        artc::AsbScript parsed;
        Check(artc::ParseIetScript(
                  "*adv_save\n"
                  "[ui_return]\t// uiに戻る\n"
                  "[ui_return]   // spaced comment\n"
                  "[btn_stop]\t\n"
                  "[if estimate=\"$t.x == 1\"]// tight comment\n"
                  "[r]本文 // not a tag line\n",
                  &parsed),
              "parse iet with trailing comments");
        Check(parsed.lines.size() == 6, "six lines parsed");
        Check(parsed.lines[0].is_label && parsed.lines[0].command == "adv_save",
              "label line");
        Check(parsed.lines[1].command == "ui_return" && parsed.lines[1].attrs.empty(),
              "tab-comment tag line");
        Check(parsed.lines[2].command == "ui_return", "space-comment tag line");
        Check(parsed.lines[3].command == "btn_stop", "trailing-tab tag line");
        Check(parsed.lines[4].command == "if", "tight-comment tag line");
        Check(parsed.lines[5].command == "\x01""TEXT",
              "text after a tag bracket stays scenario text");
    }
    Check(e3_private_pass,"Pass private nonloop fades exactly 20 frames once, without finishing globals or loops");
    std::cout << "audio channels and wait regressions passed\n";
}
