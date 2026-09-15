// native_activity.cpp — NativeActivity entry for the clean-room compat engine.
//
// Lifecycle design (standard native-activity pattern, app-glue style):
//   * ANativeActivity_onCreate registers callbacks and spawns ONE engine thread
//   * the engine thread runs the boot sequence (packs -> config -> Lua ->
//     first.iet) and then the render loop
//   * window callbacks only manage window ownership (ANativeWindow_acquire on
//     create, transfer/release on destroy) — ALL EGL calls happen on the engine
//     thread, since an EGL context is thread-bound
//   * input queue events are drained on a dedicated looper thread so the main
//     thread never ANRs
//
// All progress goes to logcat under the "Artemis" tag.
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/input.h>
#include <android/looper.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <dirent.h>
#include <jni.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "log/logger.h"
#include "core/engine_context.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "script/asb_parser.h"
#include "script/dialog_request.h"
#include "script/iet_interpreter.h"
#include "config/ini.h"
#include "render/renderer.h"
#include "render/compositor.h"

#define TAG "Artemis"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using namespace artc;

namespace {

// ---- native [dialog] input box: JNI bridge to NativeInput.java ----
JavaVM *g_vm = nullptr;
jclass g_input_class = nullptr;          // global ref, com.ies_net.artemis.debug.NativeInput
jmethodID g_input_install = nullptr;     // install(Activity)
jmethodID g_input_show = nullptr;        // show(String,String,String,int,int)
jmethodID g_input_is_done = nullptr;     // boolean isDone()
jmethodID g_input_result_ok = nullptr;   // boolean resultOk()
jmethodID g_input_result_text = nullptr; // String resultText()

// Resolve NativeInput through the Activity's class loader first (robust when
// the .so is dlopen'd from an external plugin dir by a loader stub).
jclass ResolveNativeInputClass(JNIEnv *env, ANativeActivity *activity) {
    if (activity && activity->clazz) {
        jclass actCls = env->GetObjectClass(activity->clazz);
        if (actCls) {
            jmethodID getCl = env->GetMethodID(
                actCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
            jclass clCls = nullptr;
            jmethodID loadClass = nullptr;
            if (getCl) {
                jobject cl = env->CallObjectMethod(activity->clazz, getCl);
                if (!env->ExceptionCheck() && cl) {
                    clCls = env->GetObjectClass(cl);
                    if (clCls)
                        loadClass = env->GetMethodID(
                            clCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
                    if (loadClass) {
                        jstring nm = env->NewStringUTF("com.ies_net.artemis.debug.NativeInput");
                        jclass cls = static_cast<jclass>(
                            env->CallObjectMethod(cl, loadClass, nm));
                        env->DeleteLocalRef(nm);
                        if (!env->ExceptionCheck() && cls) {
                            env->DeleteLocalRef(clCls);
                            env->DeleteLocalRef(cl);
                            env->DeleteLocalRef(actCls);
                            return cls;
                        }
                        if (env->ExceptionCheck()) env->ExceptionClear();
                    }
                    if (clCls) env->DeleteLocalRef(clCls);
                    env->DeleteLocalRef(cl);
                }
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(actCls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    jclass cls = env->FindClass("com/ies_net/artemis/debug/NativeInput");
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return cls;
}

// Cache the bridge class/methods on the UI thread (ANativeActivity_onCreate),
// where class resolution finds the app's classes.
void CacheNativeInput(ANativeActivity *activity) {
    if (!activity || !activity->vm) return;
    g_vm = activity->vm;
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK)
        return;
    jclass local = ResolveNativeInputClass(env, activity);
    if (!local) {
        LOGI("NativeInput.java not present; [dialog] will be treated as cancelled");
        return;
    }
    g_input_class = static_cast<jclass>(env->NewGlobalRef(local));
    env->DeleteLocalRef(local);
    g_input_install = env->GetStaticMethodID(
        g_input_class, "install", "(Landroid/app/Activity;)V");
    g_input_show = env->GetStaticMethodID(
        g_input_class, "show", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;II)V");
    g_input_is_done = env->GetStaticMethodID(g_input_class, "isDone", "()Z");
    g_input_result_ok = env->GetStaticMethodID(g_input_class, "resultOk", "()Z");
    g_input_result_text = env->GetStaticMethodID(
        g_input_class, "resultText", "()Ljava/lang/String;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (g_input_install && activity->clazz)
        env->CallStaticVoidMethod(g_input_class, g_input_install, activity->clazz);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Blocking host handler (runs on the engine thread): post the dialog to the UI
// thread, then poll until it is dismissed. The engine is paused meanwhile.
bool ShowNativeInput(artc::DialogRequest &req) {
    if (!g_vm || !g_input_class || !g_input_show) {
        req.text.clear();
        req.accepted = false;
        return false;
    }
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (g_vm->AttachCurrentThread(&env, nullptr) != JNI_OK) return false;
        attached = true;
    }
    jstring jt = env->NewStringUTF(req.title.c_str());
    jstring jm = env->NewStringUTF(req.message.c_str());
    jstring jd = env->NewStringUTF("");
    const jint mode = req.has_input ? 2 : (req.has_result ? 1 : 0);
    env->CallStaticVoidMethod(g_input_class, g_input_show, jt, jm, jd,
                              static_cast<jint>(req.textfieldsize), mode);
    env->DeleteLocalRef(jt);
    env->DeleteLocalRef(jm);
    env->DeleteLocalRef(jd);
    if (env->ExceptionCheck()) env->ExceptionClear();
    // Poll for the user (touch input goes to the dialog window, not the engine).
    for (int i = 0; i < 20000; ++i) {
        if (env->CallStaticBooleanMethod(g_input_class, g_input_is_done) == JNI_TRUE)
            break;
        if (env->ExceptionCheck()) { env->ExceptionClear(); break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    const jboolean ok = env->CallStaticBooleanMethod(g_input_class, g_input_result_ok);
    if (env->ExceptionCheck()) env->ExceptionClear();
    jstring res = static_cast<jstring>(
        env->CallStaticObjectMethod(g_input_class, g_input_result_text));
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (res) {
        const char *utf = env->GetStringUTFChars(res, nullptr);
        req.text = utf ? utf : "";
        if (utf) env->ReleaseStringUTFChars(res, utf);
        env->DeleteLocalRef(res);
    } else {
        req.text.clear();
    }
    req.accepted = (ok == JNI_TRUE);
    if (attached) g_vm->DetachCurrentThread();
    return req.accepted;
}

struct EngineState {
    std::atomic<bool> running{false};
    std::thread worker;
    std::thread input_thread;
    std::atomic<bool> input_running{false};
    AInputQueue *input_queue = nullptr;

    // window ownership: callbacks acquire/transfer; the engine thread releases
    std::mutex mutex;
    ANativeWindow *wanted_window = nullptr;   // acquired ref, not yet adopted
    ANativeWindow *current_window = nullptr;  // ref held by the engine thread
    ANativeWindow *lost_window = nullptr;     // destroyed, pending release after
                                              // the renderer shuts down
    artc::Renderer renderer;

    // Engine graph (packs/ini/audio/compositor/lua/runner) survives boot so
    // the frame loop can drive onEnterFrame and feed input.
    // Engine-thread-only; created in EngineThreadMain.
    std::unique_ptr<artc::EngineContext> ctx;
    int stage_w = 1280, stage_h = 720;
    int win_w = 0, win_h = 0;

    // normalized input events, produced on the looper thread, drained per frame
    struct RawInput {
        bool is_key;      // true = key press/release, false = touch
        bool is_move;     // true = touch motion (no edge, drag continuation)
        int key;          // official key id (1 = tap, 8 = back, ...)
        bool down;        // press/release edge
        float x, y;       // touch position in window pixels
    };
    std::mutex input_mutex;
    std::deque<RawInput> input_queue_events;
};

EngineState g_state;

std::vector<std::string> ListPacks(const char *dir) {
    std::vector<std::string> found;
    if (!dir) return found;
    DIR *d = opendir(dir);
    if (!d) return found;
    dirent *ent;
    while ((ent = readdir(d)) != nullptr) {
        const std::string name = ent->d_name;
        if (name.size() > 4 && name.compare(name.size() - 4, 4, ".pfs") == 0)
            found.push_back(std::string(dir) + "/" + name);
    }
    closedir(d);
    return found;
}

// ---- input queue (dedicated looper thread; prevents main-thread ANR) ----

// Normalize AInputEvent into RawInput entries. Key ids follow the official
// key_id spec: touch tap = 1 (mouse left), BACK = 8 (BackSpace → BS/MWOFF),
// ENTER = 13. The engine thread converts window-pixel touch positions into
// stage coordinates when draining.
void EnqueueInput(const EngineState::RawInput &ev) {
    std::lock_guard<std::mutex> lk(g_state.input_mutex);
    g_state.input_queue_events.push_back(ev);
}

int32_t KeyEventToId(int32_t key_code) {
    switch (key_code) {
        case AKEYCODE_BACK: return 8;    // BS → MWOFF / EXIT per advkey.def
        case AKEYCODE_ENTER:
        case AKEYCODE_DPAD_CENTER: return 13;
        case AKEYCODE_DPAD_UP: return 38;
        case AKEYCODE_DPAD_DOWN: return 40;
        case AKEYCODE_DPAD_LEFT: return 37;
        case AKEYCODE_DPAD_RIGHT: return 39;
        case AKEYCODE_MENU: return 93;   // mapped to the ui layer at the framework
        case AKEYCODE_VOLUME_UP: return 137;
        case AKEYCODE_VOLUME_DOWN: return 136;
        default: return -1;
    }
}

int OnInputEvent(int, int, void *data) {
    auto *q = static_cast<AInputQueue *>(data);
    if (!q) q = g_state.input_queue;
    if (!q) return 1;
    AInputEvent *ev = nullptr;
    while (AInputQueue_getEvent(q, &ev) >= 0) {
        if (AInputQueue_preDispatchEvent(q, ev) != 0) continue;
        const int32_t type = AInputEvent_getType(ev);
        if (type == AINPUT_EVENT_TYPE_MOTION) {
            const int32_t action = AMotionEvent_getAction(ev) & AMOTION_EVENT_ACTION_MASK;
            const float x = AMotionEvent_getX(ev, 0);
            const float y = AMotionEvent_getY(ev, 0);
            if (action == AMOTION_EVENT_ACTION_DOWN ||
                action == AMOTION_EVENT_ACTION_POINTER_DOWN) {
                EnqueueInput({false, false, 1, true, x, y});
            } else if (action == AMOTION_EVENT_ACTION_MOVE) {
                // drag continuation — no key edge, updates the pointer only
                EnqueueInput({false, false, 1, true, x, y});
                g_state.input_queue_events.back().is_move = true;
                g_state.input_queue_events.back().down = false;
            } else if (action == AMOTION_EVENT_ACTION_UP ||
                       action == AMOTION_EVENT_ACTION_POINTER_UP ||
                       action == AMOTION_EVENT_ACTION_CANCEL) {
                EnqueueInput({false, false, 1, false, x, y});
            }
        } else if (type == AINPUT_EVENT_TYPE_KEY) {
            const int id = KeyEventToId(AKeyEvent_getKeyCode(ev));
            const int32_t action = AKeyEvent_getAction(ev);
            if (id > 0) {
                EnqueueInput({true, false, id, action == AKEY_EVENT_ACTION_DOWN, 0, 0});
            }
        }
        AInputQueue_finishEvent(q, ev, 1);
    }
    return 1;
}

void InputThreadMain() {
    ALooper_prepare(ALOOPER_PREPARE_ALLOW_NON_CALLBACKS);
    AInputQueue_attachLooper(g_state.input_queue, ALooper_forThread(), 1,
                             OnInputEvent, g_state.input_queue);
    while (g_state.input_running) {
        int fd, events;
        void *data;
        ALooper_pollOnce(-1, &fd, &events, &data);
    }
}

void OnInputQueueCreated(ANativeActivity *, AInputQueue *queue) {
    g_state.input_queue = queue;
    g_state.input_running = true;
    g_state.input_thread = std::thread(InputThreadMain);
    LOGI("input queue attached");
}

void OnInputQueueDestroyed(ANativeActivity *, AInputQueue *queue) {
    g_state.input_running = false;
    AInputQueue_detachLooper(queue);
    if (g_state.input_thread.joinable()) g_state.input_thread.join();
    g_state.input_queue = nullptr;
    LOGI("input queue detached");
}

// ---- engine thread: boot + render loop ----

// Boot scene: lua -> first.iet. Graphics tags route into the compositor, which
// presents through the renderer bound below. Runs on the engine thread with a
// current GL context. On success the Lua session is kept in g_state.lua so the
// frame loop can drive onEnterFrame and feed input; returns false when the
// boot script is unusable.
bool BootScene(artc::EngineContext &ctx, artc::Renderer &renderer) {
    if (!ctx.Start()) {
        LOGE("engine start failed (lua init)");
        return false;
    }
    // Native [dialog] input box (name entry): Java AlertDialog + EditText.
    ctx.lua().SetDialogHandler(ShowNativeInput);
    g_state.stage_w = ctx.stageW();
    g_state.stage_h = ctx.stageH();
    LOGI("lua ready; running boot script via iet interpreter");
    if (!ctx.BootFramework()) {
        LOGE("first.iet missing; boot aborted");
        return false;
    }
    LOGI("boot sequence finished: adv framework active");

    // M2.1 smoke scene: drive the graphics tag path with a real pack image.
    // The tag route is identical to the game's own lyc/lyprop/flip calls.
    const char *bg_candidates[] = {"image/bg/bg01a.png", "image/bg/背景1.png"};
    bool bg_found = false;
    for (const char *cand : bg_candidates) {
        if (!ctx.packs().Exists(cand)) continue;
        bg_found = true;
        const std::string sw = std::to_string(g_state.stage_w);
        const std::string sh = std::to_string(g_state.stage_h);
        const bool ok =
            ctx.lua().DispatchTag("lyc", {{"id", "bg"}, {"file", cand}}) &&
            ctx.lua().DispatchTag("lyprop", {{"id", "bg"},
                                             {"x", "0"}, {"y", "0"},
                                             {"w", sw}, {"h", sh}}) &&
            ctx.lua().DispatchTag("flip", {});
        if (ok) LOGI("smoke scene: %s loaded, flipped, presenting", cand);
        else LOGE("smoke scene: %s tag dispatch failed", cand);
        break;
    }
    if (!bg_found) LOGI("smoke scene: no known bg image in packs; skip");

    // M2.2 demo text cycler removed: with the M3 story loop live the same
    // taps must drive the framework's message pipeline, not a demo overlay.
    return true; // layers stay on the compositor; the frame loop keeps presenting
}

void EngineThreadMain(ANativeActivity *activity) {
    // 1) locate a pack chain in the app data dirs
    std::string pack_path;
    for (const char *base :
         {activity->externalDataPath, activity->internalDataPath}) {
        for (const std::string &p : ListPacks(base)) {
            PackManager probe;
            if (probe.OpenChain(p, {})) { pack_path = p; break; }
        }
        if (!pack_path.empty()) break;
        LOGI("no pf8 chain under %s", base ? base : "(null)");
    }
    if (pack_path.empty()) {
        LOGE("no pf8 pack chain found in app data dirs; place the game's "
             "root.pfs/reaanidt.pfs under Android/data/<pkg>/files/");
        return;
    }
    LOGI("pack chain base: %s", pack_path.c_str());

    // 2) boot + render loop — everything GL lives on this thread.
    // The engine graph (packs/ini/audio/compositor/lua/runner) is owned by
    // one EngineContext; this host only drives it.
    auto ctx = std::make_unique<artc::EngineContext>();
    if (!ctx->Open(pack_path, "android")) {
        LOGE("engine open failed for %s", pack_path.c_str());
        return;
    }
    // Publish for the JNI shims (audio pause hooks) — registry, not ownership.
    artc::EngineContext::SetCurrent(ctx.get());

    artc::Renderer renderer;
    bool booted = false;
    uint64_t drawn_rev = ~0ull;   // redraw gate: last presented layer revision

    while (g_state.running) {
        ANativeWindow *wanted, *current, *lost;
        {
            std::lock_guard<std::mutex> lk(g_state.mutex);
            wanted = g_state.wanted_window;
            current = g_state.current_window;
            lost = g_state.lost_window;
            g_state.lost_window = nullptr;
        }
        if (wanted != current) {
            renderer.Shutdown();
            ctx->compositor().ReleaseGl();  // old context is gone; textures die with it
            booted = false;                 // re-boot the scene on the new context
            drawn_rev = ~0ull;              // new surface: force a first draw
            ctx->ResetSession();
            if (current) ANativeWindow_release(current);
            current = wanted;
            if (current && !renderer.Init(current))
                LOGE("renderer init failed: %s", renderer.LastError().c_str());
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.current_window = current;
        }
        if (lost) ANativeWindow_release(lost);

        // [reset] tag = engine reboot (language-select flow ends this way):
        // drop the Lua session so the boot chain re-runs on the next frame.
        if (ctx->Started() && ctx->lua().ConsumeResetRequest()) {
            LOGI("engine reset requested; re-running boot chain");
            ctx->compositor().ReleaseGl();
            booted = false;
            drawn_rev = ~0ull;
            ctx->ResetSession();
        }
        // [exit] tag = the framework's go_exit (title exit → dialog YES):
        // terminate the app from the game thread.
        if (ctx->Started() && ctx->lua().ConsumeExitRequest()) {
            LOGI("engine exit requested; finishing activity");
            ANativeActivity_finish(activity);
            g_state.running = false;
            break;
        }

        if (renderer.Ready()) {
            g_state.win_w = renderer.Width();
            g_state.win_h = renderer.Height();
            if (!booted) {
                renderer.SetStage(ctx->stageW(), ctx->stageH(),
                                  ctx->ini().GetInt("COMMON", "SIDECUT", 0) == 1);
                booted = BootScene(*ctx, renderer);
                if (!booted) break; // nothing to present; stop the thread
            }
            artc::LuaEngine &lua = ctx->lua();
            artc::AsbRunner &runner = ctx->runner();
            // Drain the enqueueTag queue at a command boundary (after each
            // script command AND after input-dispatched calllua). The
            // original engine processes the queue right after every
            // command; a queued "wait" (from eqwait) engages the wait
            // here, which stops further draining.
            auto drain = [&]() {
                while (!lua.IsWaiting() && lua.HasQueuedTag()) {
                    std::string name;
                    std::vector<std::pair<std::string, std::string>> attrs;
                    if (!lua.PopQueuedTag(&name, &attrs)) break;
                    if (name == "jump" || name == "call") {
                        std::string file, label;
                        for (const auto &kv : attrs) {
                            if (kv.first == "file") file = kv.second;
                            else if (kv.first == "label") label = kv.second;
                        }
                        LOGI("queued [%s] file=%s label=%s",
                             name.c_str(), file.c_str(), label.c_str());
                        runner.Jump(file, label);
                    } else {
                        lua.DispatchTag(name, attrs, false);
                    }
                }
            };
            // 1) drain queued input into the Lua-side key state
            if (ctx->Started()) {
                std::deque<EngineState::RawInput> batch;
                {
                    std::lock_guard<std::mutex> lk(g_state.input_mutex);
                    batch.swap(g_state.input_queue_events);
                }
                int touch_count = 0;
                bool tapped = false;
                bool was_dragging = false;
                float tap_x = 0, tap_y = 0;
                for (const auto &ev : batch) {
                    if (ev.is_key) {
                        if (ev.down) lua.PushKeyDown(ev.key);
                        else lua.PushKeyUp(ev.key);
                    } else if (ev.is_move) {
                        // drag continuation: move the pinned (draggable) layer
                        lua.SetMousePoint(renderer.StageX(ev.x),
                                          renderer.StageY(ev.y));
                        lua.DragMove(renderer.StageX(ev.x),
                                     renderer.StageY(ev.y));
                    } else {
                        lua.SetMousePoint(renderer.StageX(ev.x), renderer.StageY(ev.y));
                        if (ev.down) {
                            lua.PushKeyDown(1); // tap = key id 1
                            touch_count = 1;
                            lua.BeginDrag(renderer.StageX(ev.x),
                                          renderer.StageY(ev.y));
                        } else {
                            lua.PushKeyUp(1);
                            touch_count = 0;
                            was_dragging = lua.DragMoved();
                            lua.EndDrag();
                            if (!was_dragging) {   // a clean tap, not a drag
                                tapped = true;
                                tap_x = ev.x;
                                tap_y = ev.y;
                            }
                        }
                        lua.SetTouchCount(touch_count);
                    }
                }
                // M3: hit-test taps against lyevent-registered layers
                if (tapped)
                    lua.ClickAt(renderer.StageX(tap_x), renderer.StageY(tap_y));
                // 2) per-frame Lua work (framework vsync polls the key state)
                lua.RunEnterFrame();
                // input-dispatched calllua may enqueue (estag call etc.)
                drain();
            }
            // 2) native script execution (compiled .asb tag stack)
            if (runner.Loaded() && !runner.Halted() && !lua.IsWaiting()) {
                for (int steps = 0; steps < 4 && runner.Loaded() &&
                                    !runner.Halted(); ++steps) {
                    // Lua may jump/call/return while the instruction runs;
                    // ExecuteLine does not retain references into the script.
                    runner.ExecuteLine(lua);
                    // command-boundary queue processing (estag chains)
                    drain();
                    if (lua.IsWaiting()) break;
                }
            } else if (!lua.IsWaiting() && lua.HasQueuedTag()) {
                // runner not loaded yet: still process queued tags (the boot
                // estag03 chain enqueued by first.iet starts the asb runner).
                drain();
            }
            // Advance [lytween] / [trans] animations to this frame's time
            // before compositing (bumps the layer revision while moving).
            ctx->compositor().Update(lua.NowMs());
                // 3) present the current layer state — redraw gate (T2-2):
                // a fully static scene (no revision change, no pending tween/
                // text/transition) skips clear+draw+swap entirely.
            const double now = lua.NowMs();
            const bool animating =
                ctx->compositor().PendingAnimationMs(now) > 0 ||
                ctx->compositor().TransitionActive() ||
                ctx->compositor().PendingTextMs(now) > 0;
            if (drawn_rev != ctx->compositor().Revision() || animating) {
                renderer.Clear();   // clears the full surface (letterbox bars too)
                ctx->compositor().Draw();  // layers (script [flip] draws only)
                renderer.Present(); // single present per frame — no flicker
                drawn_rev = ctx->compositor().Revision();
            }
            lua.EndFrame();     // clear per-frame edges
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    // exit: release GL state and window refs
    ctx->Shutdown();
    renderer.Shutdown();
    {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        if (g_state.current_window) {
            ANativeWindow_release(g_state.current_window);
            g_state.current_window = nullptr;
        }
        if (g_state.lost_window) {
            ANativeWindow_release(g_state.lost_window);
            g_state.lost_window = nullptr;
        }
    }
}

} // namespace

extern "C" JNIEXPORT void JNICALL
ANativeActivity_onCreate(ANativeActivity *activity, void *savedState,
                         size_t savedStateSize) {
    activity->callbacks->onStart = [](ANativeActivity *) {};
    activity->callbacks->onResume = [](ANativeActivity *) {};
    activity->callbacks->onPause = [](ANativeActivity *) {};
    activity->callbacks->onStop = [](ANativeActivity *) {};
    activity->callbacks->onDestroy = [](ANativeActivity *) {
        LOGI("compat engine: onDestroy");
        g_state.running = false;
        if (g_state.worker.joinable()) g_state.worker.join();
    };
    activity->callbacks->onNativeWindowCreated = [](ANativeActivity *,
                                                    ANativeWindow *window) {
        ANativeWindow_acquire(window);
        {
            std::lock_guard<std::mutex> lk(g_state.mutex);
            g_state.wanted_window = window;
        }
        LOGI("native window created");
    };
    activity->callbacks->onNativeWindowDestroyed = [](ANativeActivity *,
                                                      ANativeWindow *window) {
        std::lock_guard<std::mutex> lk(g_state.mutex);
        if (g_state.wanted_window == window) g_state.wanted_window = nullptr;
        if (g_state.current_window == window) {
            g_state.lost_window = window;
            g_state.current_window = nullptr;
        }
        LOGI("native window destroyed");
    };
    activity->callbacks->onNativeWindowResized = nullptr;
    activity->callbacks->onNativeWindowRedrawNeeded = nullptr;
    activity->callbacks->onContentRectChanged = nullptr;
    activity->callbacks->onConfigurationChanged = nullptr;
    activity->callbacks->onLowMemory = nullptr;
    activity->callbacks->onSaveInstanceState = nullptr;
    activity->callbacks->onInputQueueCreated = OnInputQueueCreated;
    activity->callbacks->onInputQueueDestroyed = OnInputQueueDestroyed;
    activity->instance = nullptr;

    CacheNativeInput(activity);

    g_state.running = true;
    LOGI("compat engine: ANativeActivity_onCreate (clean-room build)");
    g_state.worker = std::thread(EngineThreadMain, activity);
}
