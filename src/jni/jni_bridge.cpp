// jni_bridge.cpp — Android host JNI entry points.
//
// Naming note: the package `com.ies_net.artemis` contains an underscore, which
// JNI escapes as `_1` in symbol names (Java_com_ies_1net_artemis_...).
//
// Native signatures and host operations (some callbacks remain logging stubs):
//   ExecuteTag(String)                      Java -> engine: run one script tag
//   EmulateKeyEvent(int key, int status)    Java -> engine: inject key event
//   OnFinishVideo()                         Java -> engine: video finished
//   OnFinishPurchase(int, String×4, int, String)  purchase finished
//   OnReadyPlayAssetDelivery(String)        Java -> engine: PAD assets ready
//   OnReadyPlayAssetDelivery(int, int, int)  exported with the __III suffix
//   moe.artemis.gui.Dialog.OnClose(int, String, long)  dialog closed
// The PAD short/long signatures differ between the official Java shell (String)
// and Tyranor-Next's Kotlin Activity (Int, Int, Int). ART resolves the short
// name first, so JNI_OnLoad additionally binds both descriptors explicitly with
// RegisterNatives (see RegisterActivityNatives); the dynamic exports stay
// unchanged. See docs/embedding.md.
//
// Audio lifecycle hooks: the official libartemis.so exports
//   artemis::CSoundDevice::PauseAllInstance() / ResumeAllInstance()
// (_ZN7artemis12CSoundDevice16PauseAllInstanceEv / ...17ResumeAllInstanceEv)
// and Tyranor's libartemis_audio_bridge.so dlsym()s ONLY those mangled names.
// They are exported here and forward to the same code as the legacy unmangled
// PauseAllInstance / ResumeAllInstance, which are kept for existing consumers.
//
// M0: symbols exist, log activity, and delegate to the engine core where the
// subsystem exists (tag text is forwarded to the Lua `e:tag` path).
#include "core/engine_context.h"
#include "jni/jni_util.h"
#include "jni/host_lifecycle.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "util/byteutil.h"

#include <jni.h>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

using namespace artc;

namespace {
// Single engine graph, owned by EngineContext (no raw g_packs/g_lua globals).
// The headless DebugBridge bootstrap has no GL context, so it uses
// EngineContext::Start(false) — no compositor.
std::unique_ptr<EngineContext> g_ctx;
// Only this bridge owns g_ctx. JNI calls can arrive on different Java threads;
// serialize replacement, script work and direct headless audio operations.
std::mutex g_host_mtx;
bool g_debug_audio_paused = false;

// Caller holds g_host_mtx; never use the NativeActivity Current registry.
bool EngineReady() { return g_ctx && g_ctx->Started(); }

void ApplyAudioPause(bool paused) {
    std::lock_guard<std::mutex> lock(g_host_mtx);
    RequestAudioPause(paused); // NativeActivity still consumes intent on its worker.
    g_debug_audio_paused = paused;
    if (EngineReady()) {
        if (paused) g_ctx->lua().PauseAudio();
        else g_ctx->lua().ResumeAudio();
    }
}
} // namespace

extern "C" {

// Java: private void ExecuteTag(String data)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_ExecuteTag(JNIEnv *env, jobject, jstring data) {
    std::string s;
    if (!artc::jni::JStringToStd(env, data, &s)) {
        artc::Log(kLogInfo, "ExecuteTag: null string ignored");
        return;
    }
    artc::Log(kLogInfo, "ExecuteTag: " + s);
    std::lock_guard<std::mutex> lock(g_host_mtx);
    // M0: route through the Lua bridge tag table so script-visible state stays
    // in sync; M1 replaces this with the native tag dispatcher.
    if (EngineReady()) {
        lua_State *L = g_ctx->lua().state();
        lua_getglobal(L, "e");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "tag");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);           // self
                // wrap the tag text into {data} — real parsing arrives with M1
                lua_newtable(L);
                lua_pushlstring(L, s.data(), s.size());
                lua_rawseti(L, -2, 1);
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    artc::Log(kLogError, std::string("ExecuteTag lua: ") + lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
        }
        lua_settop(L, 0);
    }
}

// Java: protected void EmulateKeyEvent(int key, int status)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_EmulateKeyEvent(JNIEnv *, jobject, jint key,
                                                          jint status) {
    artc::Log(kLogInfo, "EmulateKeyEvent: key=" + std::to_string(key) +
                            " status=" + std::to_string(status));
}

// Java: private void OnFinishVideo()
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_OnFinishVideo(JNIEnv *, jobject) {
    artc::Log(kLogInfo, "OnFinishVideo");
}

// Java: private void OnFinishPurchase(int result, String title, String desc,
//                                     String price, String token,
//                                     int errorResponse, String errorMessage)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_OnFinishPurchase(JNIEnv *env, jobject, jint result,
                                                           jstring title, jstring desc,
                                                           jstring price, jstring token,
                                                           jint error_response,
                                                           jstring error_message) {
    auto str = [env](jstring s) { return artc::jni::JStringOr(env, s); };
    artc::Log(kLogInfo, "OnFinishPurchase: result=" + std::to_string(result) +
                            " title=" + str(title) + " err=" + str(error_message));
}

// Java: private void OnReadyPlayAssetDelivery(String assetPaths)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery(JNIEnv *env, jobject,
                                                                   jstring asset_paths) {
    std::string s;
    if (!artc::jni::JStringToStd(env, asset_paths, &s)) {
        artc::Log(kLogInfo, "OnReadyPlayAssetDelivery: null string ignored");
        return;
    }
    artc::Log(kLogInfo, "OnReadyPlayAssetDelivery: " + s);
}

} // extern "C"

// Java: public void OnClose(int result, String text, long context)
// (declared in moe.artemis.gui.Dialog — an instance method of the dialog object)
extern "C" JNIEXPORT void JNICALL
Java_moe_artemis_gui_Dialog_OnClose(JNIEnv *env, jobject, jint result, jstring text,
                                    jlong context) {
    artc::Log(kLogInfo, "Dialog.OnClose: result=" + std::to_string(result) +
                            " ctx=" + std::to_string(context) +
                            " text=" + artc::jni::JStringOr(env, text));
}

// Java: public static native void nativeInstall(String packDir, String keyHex)
// Bootstrap used by our test shell (and optionally by the template's
// Application class) to point the engine at the data directory.
extern "C" JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_debug_DebugBridge_nativeInstall(JNIEnv *env, jclass,
                                                          jstring pack_dir_j,
                                                          jstring key_hex_j) {
    auto str = [env](jstring s) { return artc::jni::JStringOr(env, s); };
    const std::string dir = str(pack_dir_j);
    const std::string key_hex = str(key_hex_j);
    std::vector<uint8_t> key;
    if (!artc::ParseHexKey(key_hex, key)) {
        artc::Log(kLogError, "nativeInstall: bad key hex");
        return;
    }
    std::lock_guard<std::mutex> lock(g_host_mtx);
    // Headless assembly: no GL context on this path, so no compositor
    // (matches the historical DebugBridge behavior exactly).
    g_ctx = std::make_unique<artc::EngineContext>();
    if (!g_ctx->Open(dir, "android", key)) {
        artc::Log(kLogError, "nativeInstall: cannot open pack chain at " + dir);
        g_ctx.reset();
        return;
    }
    if (!g_ctx->Start(/*with_compositor=*/false)) {
        artc::Log(kLogError, "nativeInstall: engine start failed");
        g_ctx.reset();
        return;
    }
    if (g_debug_audio_paused) g_ctx->lua().PauseAudio();
    artc::EngineContext::SetCurrent(g_ctx.get());
    artc::Log(kLogInfo, "nativeInstall: engine ready, packs=" +
                            std::to_string(g_ctx->packs().Packs().size()));
    std::string err;
    if (g_ctx->lua().RunPackScript("system/init.lua", &err))
        artc::Log(kLogInfo, "init.lua executed");
    else
        artc::Log(kLogError, "init.lua failed: " + err);
}

// Kotlin shell overload: ArtemisActivity.OnReadyPlayAssetDelivery(a: Int, b: Int, c: Int)
extern "C" JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery__III(JNIEnv *, jobject,
                                                                        jint a, jint b, jint c) {
    artc::Log(kLogInfo, "OnReadyPlayAssetDelivery(III): " + std::to_string(a) + "," +
                            std::to_string(b) + "," + std::to_string(c));
}

// Host-shell lifecycle audio hooks publish worker intent and protect the
// bridge-owned headless mixer. Shared by mangled and unmangled exports.
namespace {
void PauseAllVoices() {
    ApplyAudioPause(true);
}
void ResumeAllVoices() {
    ApplyAudioPause(false);
}
} // namespace

// Official symbol names: artemis::CSoundDevice::PauseAllInstance() and
// ResumeAllInstance(). Tyranor's audio bridge resolves exactly these two
// mangled names with dlsym(RTLD_DEFAULT, ...). Defined as real static member
// functions so the compiler produces the Itanium mangling
// (_ZN7artemis12CSoundDevice16PauseAllInstanceEv / ...17ResumeAllInstanceEv).
namespace artemis {
struct CSoundDevice {
    __attribute__((visibility("default"))) static void PauseAllInstance();
    __attribute__((visibility("default"))) static void ResumeAllInstance();
};
void CSoundDevice::PauseAllInstance() {
    artc::Log(kLogInfo, "CSoundDevice::PauseAllInstance");
    PauseAllVoices();
}
void CSoundDevice::ResumeAllInstance() {
    artc::Log(kLogInfo, "CSoundDevice::ResumeAllInstance");
    ResumeAllVoices();
}
} // namespace artemis

// Legacy unmangled exports (kept for existing consumers).
extern "C" __attribute__((visibility("default"))) void
PauseAllInstance() {
    artc::Log(kLogInfo, "PauseAllInstance");
    PauseAllVoices();
}
extern "C" __attribute__((visibility("default"))) void
ResumeAllInstance() {
    artc::Log(kLogInfo, "ResumeAllInstance");
    ResumeAllVoices();
}

// Explicit bindings for ArtemisActivity.OnReadyPlayAssetDelivery. ART resolves
// the short JNI name first, so a Kotlin `external fun (Int, Int, Int)` would
// bind to the (String) export above. RegisterNatives attaches each descriptor
// to its own function. Each registration is independent and best-effort: a
// missing class or descriptor (e.g. the official shell has no (III) variant,
// or the library was loaded by an unrelated class loader) is cleared and
// ignored, and JNI_OnLoad never fails because of it.
//
// Risk: FindClass initialises ArtemisActivity if it is not initialised yet,
// running its static initialiser at library-load time. When the Activity loads
// the library from its own <clinit> this is a same-thread recursion, which the
// VM allows.
namespace {
void RegisterActivityNatives(JavaVM *vm) {
    JNIEnv *env = nullptr;
    if (!vm || vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK || !env)
        return;
    jclass cls = env->FindClass("com/ies_net/artemis/ArtemisActivity");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!cls) {
        artc::Log(kLogInfo, "JNI_OnLoad: ArtemisActivity not visible; PAD natives not registered");
        return;
    }
    struct Entry {
        const char *sig;
        void *fn;
    };
    const Entry entries[] = {
        {"(Ljava/lang/String;)V",
         reinterpret_cast<void *>(
             &Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery)},
        {"(III)V",
         reinterpret_cast<void *>(
             &Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery__III)},
    };
    for (const Entry &e : entries) {
        const JNINativeMethod m = {"OnReadyPlayAssetDelivery", e.sig, e.fn};
        const jint rc = env->RegisterNatives(cls, &m, 1);
        if (env->ExceptionCheck()) env->ExceptionClear();
        artc::Log(kLogInfo, std::string("JNI_OnLoad: RegisterNatives OnReadyPlayAssetDelivery") +
                                e.sig + (rc == JNI_OK ? " ok" : " skipped"));
    }
    env->DeleteLocalRef(cls);
}
} // namespace

jint JNI_OnLoad(JavaVM *vm, void *) {
    artc::Log(kLogInfo, "compat engine: JNI_OnLoad");
    RegisterActivityNatives(vm);
    return JNI_VERSION_1_6;
}
