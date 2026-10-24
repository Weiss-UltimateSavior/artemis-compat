// apple_audio_regressions — behavior checks for the Apple (AudioUnit) audio
// backend, built and run only when the core is configured with
// ARTC_AUDIO_BACKEND=APPLE (macOS desktop is the supported regression host).
//
// Fixtures are synthesized sine streams (no game assets); assertions use only
// the engine's queryable state (PlayStream/IsPlaying/PlaybackMs/Stop), never
// acoustics, and mirror the OHAudio semantics documented in
// src/platform/ohos/audio_ohos.cpp:
//   - IsPlaying turns false when the rendered clock passes the submitted
//     frames (data exhausted; looping aside),
//   - PlaybackMs is a real presentation clock (-1 only without one),
//   - PauseAll freezes presentation and holds new voices unstarted,
//   - Stop/StopAll remove voices and their clocks, Shutdown is idempotent.
// A default output device is required (AudioUnit create fails without one).
#include "audio/audio.h"
#include "audio/pcm_stream.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

void Check(bool ok, const char *message) {
    if (!ok) {
        std::cerr << "apple_audio_regressions: " << message << '\n';
        std::exit(1);
    }
}

using Clock = std::chrono::steady_clock;

long MsSince(const Clock::time_point &start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - start).count();
}

// Polls until the predicate holds or the timeout passes; elapsed ms, or -1.
template <typename Pred>
long WaitUntil(Pred pred, long timeout_ms) {
    const auto start = Clock::now();
    while (MsSince(start) < timeout_ms) {
        if (pred()) return MsSince(start);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return -1;
}

// Interleaved-stereo 440 Hz sine (PcmStream contract: S16, stereo).
class SineStream : public artc::PcmStream {
public:
    SineStream(int sample_rate, uint64_t frames) : sr_(sample_rate), total_(frames) {}
    size_t ReadStereo(int16_t *out, size_t frames) override {
        size_t written = 0;
        while (written < frames && pos_ < total_) {
            const double t = 2.0 * 3.14159265358979323846 * 440.0 *
                             static_cast<double>(pos_) / static_cast<double>(sr_);
            const auto s = static_cast<int16_t>(8000.0 * std::sin(t));
            out[2 * written] = s;
            out[2 * written + 1] = s;
            ++pos_;
            ++written;
        }
        return written;
    }
    int SampleRate() const override { return sr_; }
    uint64_t FrameCount() const override { return total_; }
    bool Ended() const override { return pos_ >= total_; }

private:
    int sr_;
    uint64_t total_;
    uint64_t pos_ = 0;
};

} // namespace

int main() {
    constexpr int kRate = 48000;
    artc::Audio audio;
    audio.Init(nullptr);

    // 1. Play -> IsPlaying flips from true to false once the data is presented.
    {
        auto s = std::make_unique<SineStream>(kRate, kRate / 4);  // 250 ms
        Check(audio.PlayStream("tone", std::move(s), 1000), "play a synthesized stream");
        Check(audio.IsPlaying("tone"), "voice reports playing right after Play");
        const long waited = WaitUntil([&] { return !audio.IsPlaying("tone"); }, 5000);
        Check(waited >= 0, "IsPlaying turns false once the stream is exhausted");
        Check(waited >= 100, "the stream was actually presented, not instant-finished");
        Check(audio.PlaybackMs("tone") >= 0, "presentation clock survives the tail");
    }

    // 2. PlaybackMs is non-negative and monotonic while presenting.
    {
        auto s = std::make_unique<SineStream>(kRate, kRate);  // 1 s
        Check(audio.PlayStream("clock", std::move(s), 800), "play for clock checks");
        double last = -1.0;
        bool advanced = false;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (audio.IsPlaying("clock") && Clock::now() < deadline) {
            const double ms = audio.PlaybackMs("clock");
            Check(ms >= 0.0, "presentation clock is available while playing");
            Check(ms >= last - 0.5, "presentation clock never moves backwards");
            advanced = advanced || ms > last + 0.5;
            last = ms;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(!audio.IsPlaying("clock"), "clock voice finished within the deadline");
        Check(advanced && last > 100.0, "presentation clock advanced with playback");
        audio.Stop("clock");
        Check(!audio.IsPlaying("clock") && audio.PlaybackMs("clock") < 0,
              "Stop removes the voice and its clock");
    }

    // 3. PauseAll freezes presentation; new voices stay unstarted; ResumeAll
    //    starts them (OHAudio parity for pause-created voices).
    {
        auto a = std::make_unique<SineStream>(kRate, kRate * 2);  // 2 s
        Check(audio.PlayStream("pausable", std::move(a), 1000), "play voice A");
        Check(WaitUntil([&] { return audio.PlaybackMs("pausable") > 50.0; }, 2000) >= 0,
              "voice A presents before the pause");
        audio.PauseAll();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        const double frozen1 = audio.PlaybackMs("pausable");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        const double frozen2 = audio.PlaybackMs("pausable");
        Check(frozen1 >= 0.0 && frozen1 == frozen2, "PauseAll freezes the presentation clock");

        auto b = std::make_unique<SineStream>(kRate, kRate / 4);  // 250 ms
        Check(audio.PlayStream("paused_new", std::move(b), 1000), "play voice B while paused");
        Check(audio.PlaybackMs("paused_new") < 0,
              "a voice created while paused has no presentation clock");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Check(audio.PlaybackMs("paused_new") < 0, "paused new voice never starts presenting");
        Check(audio.IsPlaying("paused_new"), "paused new voice is registered (OHAudio parity)");

        audio.ResumeAll();
        Check(WaitUntil([&] { return audio.PlaybackMs("paused_new") >= 0; }, 2000) >= 0,
              "ResumeAll starts voices created while paused");
        Check(WaitUntil([&] { return !audio.IsPlaying("paused_new"); }, 5000) >= 0,
              "paused voice finishes after resume");
        audio.StopAll();
    }

    // 4. Playing the same key again retires the old voice.
    {
        auto long_stream = std::make_unique<SineStream>(kRate, kRate * 2);   // 2 s
        Check(audio.PlayStream("replace", std::move(long_stream), 1000), "play long voice");
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        Check(audio.IsPlaying("replace"), "long voice is playing");
        auto short_stream = std::make_unique<SineStream>(kRate, kRate / 8);  // 125 ms
        Check(audio.PlayStream("replace", std::move(short_stream), 1000),
              "replace the voice under the same key");
        // The retired 2 s voice would still be playing at the 1.5 s deadline.
        Check(WaitUntil([&] { return !audio.IsPlaying("replace"); }, 1500) >= 0,
              "replacement retires the old voice (short one finishes)");
        Check(audio.PlaybackMs("replace") >= 0, "clock readable after the tail");
        audio.Stop("replace");
    }

    // 5. Stop/StopAll semantics and play/stop churn (with replacement overlaps).
    {
        for (int i = 0; i < 40; ++i) {
            const std::string key = "churn" + std::to_string(i % 4);
            auto s = std::make_unique<SineStream>(kRate, kRate / 8);
            Check(audio.PlayStream(key, std::move(s), 1000), "churn play");
            if (i % 2 == 0) audio.Stop(key);
        }
        audio.StopAll();
        for (const std::string &key : {"churn0", "churn1", "churn2", "churn3"}) {
            Check(!audio.IsPlaying(key) && audio.PlaybackMs(key) < 0,
                  "StopAll clears every voice");
        }
        for (int i = 0; i < 6; ++i) {  // several units rendering at once
            auto s = std::make_unique<SineStream>(kRate, kRate);
            Check(audio.PlayStream("poly" + std::to_string(i), std::move(s), 500),
                  "polyphonic play");
        }
        audio.StopAll();
    }

    // 6. Volume/pan setters on live and missing voices (state path only).
    {
        auto s = std::make_unique<SineStream>(kRate, kRate / 2);
        Check(audio.PlayStream("gain", std::move(s), 1000), "play for gain/pan setters");
        audio.SetVolume("gain", 250);
        audio.SetPan("gain", -500);
        audio.SetVolume("missing", 100);
        audio.SetPan("missing", 100);
        Check(WaitUntil([&] { return !audio.IsPlaying("gain"); }, 5000) >= 0,
              "voice with modified gain/pan finishes");
    }

    // 7. Shutdown is idempotent and leaves no playable state.
    audio.Shutdown();
    audio.Shutdown();
    Check(!audio.IsPlaying("gain"), "nothing reports playing after Shutdown");
    Check(!audio.Play("x", "x.pfs", false, 1000), "Play after Shutdown has no pack source");

    std::cout << "apple audio regressions passed\n";
}
