#pragma once
#include "render/emote_scene.h"
#include <cstdint>
#include <deque>
#include <utility>
namespace artc {
// Playback controller bridging the original Lua E-mote API to the
// EmoteModel/EmoteScene foundation. Frame semantics follow the SDK contract
// recovered from the original library: timeline positions are FRAMES at
// 60 fps, progress() takes milliseconds (clamped to [0,60000] like the
// original Update), setters accept (value, transition ms, ease) with the
// ease weight ease>=0 ? ease+1 : 1/(1-ease). Finished non-loop timelines
// HOLD their final pose until stopped, replaced or faded out — they are not
// auto-removed. Wind/outer-force physics has no renderer here; those proxy
// methods stay unregistered so the Lua boundary reports them instead of
// pretending to simulate.
class EmotePlayer {
public:
    enum : int { kTimelineParallel = 1, kTimelineSequential = 2 };
    static constexpr double kFramesPerMillisecond = 60.0 / 1000.0;
    static constexpr double kMaxProgressMs = 60000.0;

    bool Load(std::shared_ptr<const EmoteModel> model, std::string& error);
    bool Active() const { return model_ != nullptr; }

    // ---- timeline control ----
    // playTimeline restarts a playing label in place (insertion order kept);
    // the Sequential flag queues behind live (unfinished) playback instead.
    bool PlayTimeline(const std::string& label, int flags, std::string& error);
    void StopTimeline(const std::string& label);
    void StopAllTimelines();
    bool IsTimelinePlaying(const std::string& label) const;
    bool IsLoopTimeline(const std::string& label) const;
    double TimelineTotalFrames(const std::string& label) const;  // <0 unknown
    // Fades act on live playback and bypass the sequential queue; fadeOut
    // removes the timeline once its blend reaches zero.
    bool FadeInTimeline(const std::string& label, double duration_ms, int flags, std::string& error);
    bool FadeOutTimeline(const std::string& label, double duration_ms, std::string& error);
    // Direct blend set cancels a running fade (and its auto-stop).
    bool SetTimelineBlendRatio(const std::string& label, double ratio, std::string& error);
    double TimelineBlendRatio(const std::string& label, bool* found) const;
    // setTimeline(label, loop): loop=false parks a looping timeline at its
    // loop end (hold-end) instead of wrapping; loop=true resumes wrapping.
    bool SetTimelineHoldEnd(const std::string& label, bool loop, std::string& error);

    // ---- enumeration (model order for labels, insertion order for playing) ----
    int CountMainTimelines() const;
    std::string MainTimelineLabelAt(int index) const;
    int CountDiffTimelines() const;
    std::string DiffTimelineLabelAt(int index) const;
    int CountPlayingTimelines() const;
    std::string PlayingTimelineLabelAt(int index) const;
    int PlayingTimelineFlagsAt(int index) const;
    int CountVariables() const;
    std::string VariableLabelAt(int index) const;

    // ---- variables (labels come from the model; defaults are 0) ----
    bool SetVariable(const std::string& label, double value, double transition_ms,
                     double ease, std::string& error);
    double GetVariable(const std::string& label, bool* found) const;

    // ---- player transforms (applied to the container layer) ----
    void SetCoord(double x, double y, double transition_ms, double ease);
    void SetScale(double sx, double sy, double transition_ms, double ease);
    void SetRot(double degrees, double transition_ms, double ease);
    // 0xAARRGGBB like the reference setColor/getColor. Only alpha changes
    // the picture (the compositor has no RGB tint); the RGB bits round-trip.
    void SetColor(uint32_t aarrggbb, double transition_ms, double ease);
    uint32_t GetColor() const;
    void GetCoord(double* x, double* y) const;
    void GetScale(double* x, double* y) const;
    double GetRot() const;
    void SetMirror(bool mirror);
    bool IsMirrored() const { return mirror_; }
    void Show();
    void Hide();
    bool IsHidden() const { return hidden_; }

    // ---- clock ----
    void Progress(double delta_ms);
    bool IsAnimating() const;
    // Complete every transition instantly; non-loop timelines jump to their
    // final frame and hold it (looping timelines keep playing — documented
    // boundary, the original skip drives its own controller state).
    void Skip();
    // Complete transitions only, without jumping timeline positions.
    void Pass();

    // ---- rendering ----
    // SetProps on the bare container id (a texture-less holder layer the
    // compositor materializes) then evaluates the scene at the base frame.
    bool Render(Compositor& compositor, const std::string& id, std::string& error);
    // Per-frame tick: advance on the wall clock, then redraw. Returns false
    // when the scene rejected the frame; the player keeps ticking either way.
    bool Update(double now_ms, Compositor* compositor, const std::string& id);
    void RemoveLayers(Compositor& compositor, const std::string& id);

private:
    struct Animated {
        double value = 0, start = 0, target = 0, duration_ms = 0, elapsed_ms = 0, weight = 1;
        Animated() = default;
        explicit Animated(double v) : value(v) {}
        bool animating() const { return duration_ms > 0; }
        void Set(double v, double transition_ms, double ease);
        void Advance(double dt_ms);
        void Finish();
    };
    struct PlayingTimeline {
        double position = 0;       // frames
        int flags = kTimelineParallel;
        Animated blend{1};         // fades animate this; direct sets cancel it
        bool fade_out_stop = false, finished = false, hold_end = false;
    };
    const EmoteTimeline* Timeline(const std::string& label) const;
    PlayingTimeline* EnsurePlaying(const std::string& label, int flags, double blend,
                                   std::string& error);
    void StartQueued();
    bool Looping(const EmoteTimeline& timeline, bool hold_end) const;
    std::map<std::string, double> ComposeVariables() const;

    std::shared_ptr<const EmoteModel> model_;
    EmoteScene scene_;
    std::vector<std::pair<std::string, PlayingTimeline>> playing_;  // insertion order
    std::deque<std::pair<std::string, int>> queued_;                // sequential waits
    std::map<std::string, Animated> variables_;                     // model labels at 0
    Animated coord_x_, coord_y_, rot_;
    Animated scale_x_{1}, scale_y_{1}, alpha_{1};
    uint32_t color_rgb_ = 0xFFFFFF;
    double base_frame_ = 0, last_now_ms_ = -1;
    bool mirror_ = false, hidden_ = false;
};
}
