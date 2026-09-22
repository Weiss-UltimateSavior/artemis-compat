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
// The PAD short/long signatures need reconciliation with the host declarations;
// retained export names alone do not establish compatibility. See docs/embedding.md.
//
// M0: symbols exist, log activity, and delegate to the engine core where the
// subsystem exists (tag text is forwarded to the Lua `e:tag` path).
#include "core/engine_context.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "util/byteutil.h"

#include <jni.h>
#include <cstring>
#include <memory>
#include <string>

using namespace artc;

namespace {
// Single engine graph, owned by EngineContext (no raw g_packs/g_lua globals).
// The headless DebugBridge bootstrap has no GL context, so it uses
// EngineContext::Start(false) — no compositor.
std::unique_ptr<EngineContext> g_ctx;

bool EngineReady() { return g_ctx && g_ctx->Started(); }
} // namespace

jint JNI_OnLoad(JavaVM *, void *) {
    artc::Log(kLogInfo, "compat engine: JNI_OnLoad");
    return JNI_VERSION_1_6;
}

extern "C" {

// Java: private void ExecuteTag(String data)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_ExecuteTag(JNIEnv *env, jobject, jstring data) {
    const char *s = env->GetStringUTFChars(data, nullptr);
    artc::Log(kLogInfo, std::string("ExecuteTag: ") + (s ? s : ""));
    // M0: route through the Lua bridge tag table so script-visible state stays
    // in sync; M1 replaces this with the native tag dispatcher.
    if (EngineReady() && s) {
        lua_State *L = g_ctx->lua().state();
        lua_getglobal(L, "e");
        if (lua_istable(L, -1)) {
            lua_getfield(L, -1, "tag");
            if (lua_isfunction(L, -1)) {
                lua_pushvalue(L, -2);           // self
                // wrap the tag text into {data} — real parsing arrives with M1
                lua_newtable(L);
                lua_pushlstring(L, s, std::strlen(s));
                lua_rawseti(L, -2, 1);
                if (lua_pcall(L, 2, 0, 0) != 0) {
                    artc::Log(kLogError, std::string("ExecuteTag lua: ") + lua_tostring(L, -1));
                    lua_pop(L, 1);
                }
            }
        }
        lua_settop(L, 0);
    }
    if (s) env->ReleaseStringUTFChars(data, s);
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
    auto str = [env](jstring s) -> std::string {
        if (!s) return "";
        const char *p = env->GetStringUTFChars(s, nullptr);
        std::string r = p ? p : "";
        env->ReleaseStringUTFChars(s, p);
        return r;
    };
    artc::Log(kLogInfo, "OnFinishPurchase: result=" + std::to_string(result) +
                            " title=" + str(title) + " err=" + str(error_message));
}

// Java: private void OnReadyPlayAssetDelivery(String assetPaths)
JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_ArtemisActivity_OnReadyPlayAssetDelivery(JNIEnv *env, jobject,
                                                                   jstring asset_paths) {
    const char *s = env->GetStringUTFChars(asset_paths, nullptr);
    artc::Log(kLogInfo, std::string("OnReadyPlayAssetDelivery: ") + (s ? s : ""));
    if (s) env->ReleaseStringUTFChars(asset_paths, s);
}

} // extern "C"

// Java: public void OnClose(int result, String text, long context)
// (declared in moe.artemis.gui.Dialog — an instance method of the dialog object)
extern "C" JNIEXPORT void JNICALL
Java_moe_artemis_gui_Dialog_OnClose(JNIEnv *env, jobject, jint result, jstring text,
                                    jlong context) {
    const char *s = env->GetStringUTFChars(text, nullptr);
    artc::Log(kLogInfo, "Dialog.OnClose: result=" + std::to_string(result) +
                            " ctx=" + std::to_string(context) + " text=" + (s ? s : ""));
    if (s) env->ReleaseStringUTFChars(text, s);
}

// Java: public static native void nativeInstall(String packDir, String keyHex)
// Bootstrap used by our test shell (and optionally by the template's
// Application class) to point the engine at the data directory.
extern "C" JNIEXPORT void JNICALL
Java_com_ies_1net_artemis_debug_DebugBridge_nativeInstall(JNIEnv *env, jclass,
                                                          jstring pack_dir_j,
                                                          jstring key_hex_j) {
    auto str = [env](jstring s) -> std::string {
        if (!s) return "";
        const char *p = env->GetStringUTFChars(s, nullptr);
        std::string r = p ? p : "";
        env->ReleaseStringUTFChars(s, p);
        return r;
    };
    const std::string dir = str(pack_dir_j);
    const std::string key_hex = str(key_hex_j);
    std::vector<uint8_t> key;
    if (!artc::ParseHexKey(key_hex, key)) {
        artc::Log(kLogError, "nativeInstall: bad key hex");
        return;
    }
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

// Host-shell lifecycle audio hooks (ArtemisAudio dlsym lookups) — real
// mute/unmute of the active mixer voices on onPause/onResume.
extern "C" __attribute__((visibility("default"))) void
PauseAllInstance() {
    artc::Log(kLogInfo, "PauseAllInstance");
    if (auto *ctx = EngineContext::Current(); ctx && ctx->Started())
        ctx->lua().PauseAudio();
}
extern "C" __attribute__((visibility("default"))) void
ResumeAllInstance() {
    artc::Log(kLogInfo, "ResumeAllInstance");
    if (auto *ctx = EngineContext::Current(); ctx && ctx->Started())
        ctx->lua().ResumeAudio();
}
