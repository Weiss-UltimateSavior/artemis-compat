// Production JNI entry points + real EngineContext/Lua + synthetic packs.
// All Audio symbols are supplied by this test, so the static archive's real
// backend is not extracted. This does not test OpenSL playback or a real JVM.
#include "audio/audio.h"
#include "core/engine_context.h"
#include "jni/host_lifecycle.h"
#include <jni.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

extern "C" void PauseAllInstance();
extern "C" void ResumeAllInstance();
// Link the exact Itanium symbols resolved by Tyranor's audio bridge. Their
// implementations come from the production jni_bridge.cpp linked by this test.
extern "C" void PauseMangledInstance() __asm__("_ZN7artemis12CSoundDevice16PauseAllInstanceEv");
extern "C" void ResumeMangledInstance() __asm__("_ZN7artemis12CSoundDevice17ResumeAllInstanceEv");
extern "C" void Java_com_ies_1net_artemis_debug_DebugBridge_nativeInstall(JNIEnv*, jclass, jstring, jstring);
extern "C" void Java_com_ies_1net_artemis_ArtemisActivity_ExecuteTag(JNIEnv*, jobject, jstring);
namespace {
void Check(bool ok, const char* why) {
    if (!ok) { std::cerr << why << '\n'; std::exit(1); }
}
struct Mixer { bool paused=false, destroyed=false; int pauses=0, resumes=0, plays=0; bool last_play_paused=false; };
std::mutex observed_mutex;
std::vector<Mixer> mixers;
std::vector<bool> intents;
std::mutex gate_mutex;
std::condition_variable gate_cv;
bool block_pause=false, pause_entered=false, release_pause=false;
std::atomic<bool> reinstall_started{false};
Mixer State(size_t id) { std::lock_guard<std::mutex> lock(observed_mutex); return mixers.at(id); }
size_t Latest() { std::lock_guard<std::mutex> lock(observed_mutex); return mixers.size()-1; }
bool Intent() { std::lock_guard<std::mutex> lock(observed_mutex); return intents.back(); }
const char* GetString(JNIEnv*, jstring s, jboolean*) { return reinterpret_cast<const char*>(s); }
void ReleaseString(JNIEnv*, jstring, const char*) {}
jstring String(const std::string& s) { return reinterpret_cast<jstring>(const_cast<char*>(s.c_str())); }
std::filesystem::path WritePack() {
    const std::map<std::string,std::string> files={
        {"system.ini","[ANDROID]\nWIDTH=640\nHEIGHT=480\n"},
        {"system/init.lua",R"(
            local tag=e.tag
            e.tag=function(self,p)
                if p[1]=='test_sound' then tag(self,{'splay',file='synthetic.ogg'}) end
            end
        )"}
    };
    std::vector<uint8_t> bytes;
    auto put32=[&](uint32_t n){for(int i=0;i<4;++i)bytes.push_back((n>>(8*i))&255);};
    uint32_t index=4;
    for(const auto& f:files)index+=16+f.first.size();
    bytes.insert(bytes.end(),{'p','f','8'});put32(index);put32(files.size());
    uint32_t offset=7+index;
    for(const auto& f:files){
        put32(f.first.size());bytes.insert(bytes.end(),f.first.begin(),f.first.end());
        put32(0);put32(offset);put32(f.second.size());offset+=f.second.size();
    }
    for(const auto& f:files)bytes.insert(bytes.end(),f.second.begin(),f.second.end());
    const auto path=std::filesystem::temp_directory_path()/
        ("artc-debug-audio-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".pfs");
    std::ofstream stream(path,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
    return path;
}
}
namespace artc {
struct Audio::Impl { size_t id; };
Audio::Audio():impl_(new Impl) {
    std::lock_guard<std::mutex> lock(observed_mutex);
    impl_->id=mixers.size();mixers.emplace_back();
}
Audio::~Audio() {
    {std::lock_guard<std::mutex> lock(observed_mutex);mixers.at(impl_->id).destroyed=true;}
    delete impl_;
}
void Audio::Init(PackManager*) {}
void Audio::Shutdown() {}
bool Audio::Play(const std::string&,const std::string&,bool,int) {
    std::lock_guard<std::mutex> lock(observed_mutex);
    auto& s=mixers.at(impl_->id);++s.plays;s.last_play_paused=s.paused;return true;
}
bool Audio::PlayStream(const std::string&,std::unique_ptr<PcmStream>,int) {return false;}
double Audio::PlaybackMs(const std::string&) const {return -1;}
void Audio::Stop(const std::string&) {}
void Audio::StopAll() {}
bool Audio::IsPlaying(const std::string&) const {return false;}
void Audio::SetVolume(const std::string&,int) {}
void Audio::SetPan(const std::string&,int) {}
void Audio::PauseAll() {
    {std::unique_lock<std::mutex> lock(gate_mutex);
     if(block_pause){pause_entered=true;gate_cv.notify_all();gate_cv.wait(lock,[]{return release_pause;});}}
    std::lock_guard<std::mutex> lock(observed_mutex);
    auto& s=mixers.at(impl_->id);s.paused=true;++s.pauses;
}
void Audio::ResumeAll() {
    std::lock_guard<std::mutex> lock(observed_mutex);
    auto& s=mixers.at(impl_->id);s.paused=false;++s.resumes;
}
void RequestAudioPause(bool paused) {
    std::lock_guard<std::mutex> lock(observed_mutex);intents.push_back(paused);
}
}
int main() {
    JNINativeInterface functions{};
    functions.GetStringUTFChars=GetString;functions.ReleaseStringUTFChars=ReleaseString;
    JNIEnv env{&functions};
    const auto path=WritePack();const std::string pack=path.string(),key="00",sound="test_sound";
    auto install=[&](const std::string& p){Java_com_ies_1net_artemis_debug_DebugBridge_nativeInstall(&env,nullptr,String(p),String(key));};
    auto execute=[&]{Java_com_ies_1net_artemis_ArtemisActivity_ExecuteTag(&env,nullptr,String(sound));};
    struct HookPair { void (*pause)(); void (*resume)(); };
    const HookPair hooks[] = {
        {PauseAllInstance, ResumeAllInstance},
        {PauseMangledInstance, ResumeMangledInstance},
    };
    for (const auto& hook : hooks) {
        hook.pause();Check(Intent(),"each pause export publishes worker intent without a headless graph");
        hook.resume();Check(!Intent(),"each resume export publishes worker intent without a headless graph");
    }
    PauseMangledInstance();install(pack);const size_t first=Latest();
    Check(State(first).paused,"install inherits a pending pause before init.lua can play");
    execute();Check(State(first).plays==1 && State(first).last_play_paused,"ExecuteTag uses the paused owned mixer");
    ResumeAllInstance();Check(!Intent() && !State(first).paused && State(first).resumes==1,"export resumes the real headless mixer");
    {
        artc::EngineContext other;
        Check(other.Open(pack,"android",{0}) && other.Start(false),"open another engine owner");
        const size_t unrelated=Latest();artc::EngineContext::SetCurrent(&other);
        // Cross the two pause and resume families too: all four exports must
        // share owned-session state even when Current points at another owner.
        for (const auto& pause_hook : hooks) {
            for (const auto& resume_hook : hooks) {
                const auto before=State(first);
                pause_hook.pause();
                Check(Intent() && State(first).paused && State(first).pauses==before.pauses+1,
                      "each pause export reaches the bridge-owned mixer once");
                Check(State(unrelated).pauses==0 && !State(unrelated).paused,
                      "no pause export mutates a non-owned Current mixer");
                resume_hook.resume();
                Check(!Intent() && !State(first).paused && State(first).resumes==before.resumes+1,
                      "each resume export resumes the same owned mixer once");
                Check(State(unrelated).resumes==0 && !State(unrelated).paused,
                      "no resume export mutates a non-owned Current mixer");
            }
        }
    }
    install(pack);const size_t second=Latest();
    Check(State(first).destroyed && second!=first,"reinstall releases the old graph");
    PauseAllInstance();Check(State(second).paused,"reinstall receives later pause hooks");ResumeAllInstance();
    {
        std::lock_guard<std::mutex> lock(gate_mutex);block_pause=true;release_pause=false;pause_entered=false;
    }
    std::thread pauser(PauseMangledInstance);
    {std::unique_lock<std::mutex> lock(gate_mutex);
     Check(gate_cv.wait_for(lock,std::chrono::seconds(2),[]{return pause_entered;}),"pause reaches the owned mixer");}
    std::thread reinstaller([&]{reinstall_started=true;install(pack);});
    while(!reinstall_started.load())std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    Check(!State(second).destroyed,"reinstall cannot destroy a mixer in an active hook");
    {std::lock_guard<std::mutex> lock(gate_mutex);release_pause=true;block_pause=false;}gate_cv.notify_all();
    pauser.join();reinstaller.join();const size_t third=Latest();
    Check(State(second).destroyed && State(third).paused && Intent(),"serialized reinstall preserves final pause intent");
    ResumeAllInstance();Check(!Intent() && !State(third).paused,"resume state agrees with worker intent");
    const std::string missing=pack+".missing.pfs";install(missing);
    Check(State(third).destroyed,"failed install releases the old owned graph");
    for (const auto& hook : hooks) {
        hook.pause();Check(Intent(),"each pause export stays safe after a failed bootstrap");
        hook.resume();Check(!Intent(),"each resume export stays safe after a failed bootstrap");
    }
    std::filesystem::remove(path);
    std::cout<<"debug bridge audio regressions passed\n";
}
