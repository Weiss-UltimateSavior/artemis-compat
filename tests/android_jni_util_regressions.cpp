// Host-side regressions for the Android JNI layer's testable logic
// (src/jni/jni_util.h). The JNI environment is faked, so no <jni.h> is needed.
#include "jni/jni_util.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            std::cerr << "FAIL: " << msg << " (" #cond ")\n";         \
            ++g_failures;                                             \
        }                                                             \
    } while (0)

// Minimal stand-in for JNIEnv's string API. Records every Get/Release pair and
// aborts the test process if the code under test ever touches a null string.
struct FakeStr {
    const char *chars;
};
struct FakeEnv {
    int gets = 0, releases = 0;
    bool fail_get = false;
    const char *GetStringUTFChars(FakeStr *s, unsigned char *) {
        if (!s) { std::cerr << "FAIL: VM called with null jstring\n"; std::abort(); }
        ++gets;
        return fail_get ? nullptr : s->chars;
    }
    void ReleaseStringUTFChars(FakeStr *s, const char *p) {
        if (!s || !p) { std::cerr << "FAIL: Release with null\n"; std::abort(); }
        ++releases;
    }
};

void TestStringConversion() {
    FakeEnv env;
    std::string out = "stale";
    CHECK(!artc::jni::JStringToStd(&env, static_cast<FakeStr *>(nullptr), &out),
          "null jstring reports failure");
    CHECK(out.empty(), "null jstring clears output");
    CHECK(env.gets == 0, "null jstring never reaches the VM");

    FakeStr hello{"hello"};
    CHECK(artc::jni::JStringToStd(&env, &hello, &out) && out == "hello", "plain conversion");
    CHECK(env.gets == 1 && env.releases == 1, "Get/Release balanced");

    FakeStr empty{""};
    CHECK(artc::jni::JStringToStd(&env, &empty, &out) && out.empty(),
          "empty (non-null) string is success, distinct from null");

    env.fail_get = true;
    env.gets = env.releases = 0;
    CHECK(!artc::jni::JStringToStd(&env, &hello, &out), "VM returning null chars fails");
    CHECK(env.releases == 0, "no Release for a failed Get");

    env.fail_get = false;
    CHECK(artc::jni::JStringOr(&env, static_cast<FakeStr *>(nullptr)) == "", "default fallback");
    CHECK(artc::jni::JStringOr(&env, static_cast<FakeStr *>(nullptr), "(null)") == "(null)",
          "custom fallback");
    CHECK(artc::jni::JStringOr(&env, &hello, "(null)") == "hello", "value wins over fallback");
}

void TestPoll() {
    using artc::jni::PollResult;
    // Completes after 3 probes; sleeps in between.
    {
        int probes = 0, sleeps = 0;
        auto r = artc::jni::PollUntilDone(
            100, [&] { return ++probes == 3; }, [] { return false; }, [&] { ++sleeps; });
        CHECK(r == PollResult::Done, "poll done");
        CHECK(probes == 3 && sleeps == 2, "poll probes/sleeps");
    }
    // Already shutting down: returns before the first probe and never sleeps,
    // so the main thread joining the engine thread is not delayed.
    {
        int probes = 0, sleeps = 0;
        auto r = artc::jni::PollUntilDone(
            100, [&] { ++probes; return false; }, [] { return true; }, [&] { ++sleeps; });
        CHECK(r == PollResult::Aborted, "abort at entry");
        CHECK(probes == 0 && sleeps == 0, "abort at entry does no work");
    }
    // running flips to false mid-wait (onDestroy): exits within one sleep.
    {
        bool running = true;
        int probes = 0, sleeps = 0;
        auto r = artc::jni::PollUntilDone(
            20000, [&] { ++probes; return false; }, [&] { return !running; },
            [&] { if (++sleeps == 2) running = false; });
        CHECK(r == PollResult::Aborted, "abort mid-wait");
        CHECK(sleeps == 2 && probes == 2, "abort observed on the next iteration");
    }
    // Abort takes precedence over a simultaneously-finished dialog.
    {
        auto r = artc::jni::PollUntilDone(
            5, [] { return true; }, [] { return true; }, [] {});
        CHECK(r == PollResult::Aborted, "abort wins over done");
    }
    // Bounded: never spins forever.
    {
        int sleeps = 0;
        auto r = artc::jni::PollUntilDone(
            7, [] { return false; }, [] { return false; }, [&] { ++sleeps; });
        CHECK(r == PollResult::TimedOut && sleeps == 7, "timeout after max_polls");
    }
}

void TestFrameSleep() {
    using artc::jni::FrameSleepMs;
    CHECK(FrameSleepMs(false, false, false) == 16, "no window: sleep (no busy-wait)");
    CHECK(FrameSleepMs(false, true, true) == 16, "no window always sleeps");
    CHECK(FrameSleepMs(true, false, true) == 16, "idle frame sleeps");
    CHECK(FrameSleepMs(true, true, false) == 16, "unpaced swap sleeps");
    CHECK(FrameSleepMs(true, true, true) == 0, "vsync-paced drawn frame does not sleep");
}

void TestKeyConsumption() {
    using artc::jni::EngineConsumesKeyId;
    CHECK(EngineConsumesKeyId(8), "BACK is claimed by the engine");
    CHECK(EngineConsumesKeyId(13), "ENTER is claimed by the engine");
    CHECK(EngineConsumesKeyId(38), "DPAD_UP is claimed by the engine");
    CHECK(!EngineConsumesKeyId(-1), "unmapped key (incl. volume) is left to the system");
}

void TestPackSelection() {
    std::vector<std::string> packs{
        "/game/chs_patch_baijingmu.pfs",
        "/game/chs_patch_official.pfs",
        "/game/root.pfs",
        "/game/aaa.pfs",
    };
    artc::jni::SortPackPaths(packs);
    CHECK(packs[0] == "/game/root.pfs", "root pack is always the chain base");
    CHECK(packs[1] == "/game/aaa.pfs" && packs[2] == "/game/chs_patch_baijingmu.pfs" &&
              packs[3] == "/game/chs_patch_official.pfs",
          "other packs use deterministic alphabetical order");
}

} // namespace

int main() {
    TestStringConversion();
    TestPoll();
    TestFrameSleep();
    TestKeyConsumption();
    TestPackSelection();
    if (g_failures) return 1;
    std::cout << "android jni util regressions passed\n";
    return 0;
}
