// jni_util.h — host-testable helpers for the Android JNI layer.
//
// Header-only and free of <jni.h> / Android headers: the JNI types are template
// parameters, so tests can exercise the logic on a desktop host with fake
// environments. jni_bridge.cpp and native_activity.cpp instantiate them with
// the real JNIEnv.
#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

// Keep instantiations out of the shared library's dynamic export table.
#if defined(__GNUC__) || defined(__clang__)
#define ARTC_JNI_HIDDEN __attribute__((visibility("hidden")))
#else
#define ARTC_JNI_HIDDEN
#endif

namespace artc {
namespace jni {

// Copy a Java string into `out` as modified UTF-8. Returns false (and leaves
// `out` empty) when the reference is null or the VM could not provide the
// characters. Never calls into the VM with a null reference, so a Java caller
// passing null cannot crash the process.
template <class Env, class Str>
ARTC_JNI_HIDDEN bool JStringToStd(Env *env, Str s, std::string *out) {
    out->clear();
    if (!s) return false;
    const char *p = env->GetStringUTFChars(s, nullptr);
    if (!p) return false;
    out->assign(p);
    env->ReleaseStringUTFChars(s, p);
    return true;
}

// Null-tolerant convenience: null / failure yields `fallback`.
template <class Env, class Str>
ARTC_JNI_HIDDEN std::string JStringOr(Env *env, Str s, const char *fallback = "") {
    std::string r;
    return JStringToStd(env, s, &r) ? r : std::string(fallback);
}

enum class PollResult { Done, Aborted, TimedOut };

// Blocking-dialog poll used on the engine thread. `should_abort` is evaluated
// before every `is_done` probe so a shutting-down Activity (onDestroy joins the
// engine thread on the main thread) is never held up for more than one sleep.
template <class IsDone, class ShouldAbort, class Sleep>
ARTC_JNI_HIDDEN PollResult PollUntilDone(int max_polls, IsDone &&is_done, ShouldAbort &&should_abort,
                         Sleep &&sleep) {
    for (int i = 0; i < max_polls; ++i) {
        if (should_abort()) return PollResult::Aborted;
        if (is_done()) return PollResult::Done;
        sleep();
    }
    return PollResult::TimedOut;
}

// Idle wait at the end of an engine-thread iteration, in milliseconds.
//   * window lost / renderer not ready: nothing presents, so there is no swap
//     backpressure — sleep instead of spinning at 100% CPU;
//   * ready and a frame was drawn on a vsync-paced swap: the swap already
//     blocked, no sleep;
//   * ready but nothing drawn, or swap not paced: sleep.
ARTC_JNI_HIDDEN inline int FrameSleepMs(bool renderer_ready, bool drew, bool swap_paced) {
    if (!renderer_ready) return 16;
    if (!drew || !swap_paced) return 16;
    return 0;
}

// Whether the engine claims a key event (reports it handled to the input queue).
// Only keys the engine maps are claimed; unmapped keys, including the volume
// keys, are left unhandled so the system still changes the volume.
ARTC_JNI_HIDDEN inline bool EngineConsumesKeyId(int id) {
    return id > 0;
}

// NativeActivity receives a game directory, not a host-selected pack. Always
// use root.pfs as the chain base; filesystem enumeration order is not stable
// and may otherwise select a translation or compatibility patch without boot.
ARTC_JNI_HIDDEN inline void SortPackPaths(std::vector<std::string>& packs) {
    const auto is_root = [](const std::string& path) {
        if (path.size() < 9) return false;
        std::string suffix = path.substr(path.size() - 9);
        for (char& c : suffix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return suffix == "/root.pfs";
    };
    std::sort(packs.begin(), packs.end(), [&](const std::string& a, const std::string& b) {
        const bool ra = is_root(a), rb = is_root(b);
        if (ra != rb) return ra;
        return a < b;
    });
}

} // namespace jni
} // namespace artc
