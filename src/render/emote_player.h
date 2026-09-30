#pragma once
#include "render/emote_scene.h"
#include <cstdint>
#include <deque>
#include <utility>
namespace artc {
// Playback controller bridging the original Lua E-mote API to the
// EmoteModel/EmoteScene foundation. Frame semantics follow the Artemis engine
// contract recovered from the shipped scripts and the art3m1s-core binding:
// timeline positions are FRAMES at 60 fps; the Lua layer passes frame counts
// for transitions/fades/progress (lua_engine.cpp converts to the millisecond
// clock used here); setters accept (value, transition ms, ease) with the ease
// weight ease>=0 ? ease+1 : 1/(1-ease). Finished non-loop timelines HOLD their
// final pose until stopped, replaced or faded out — they are not auto-removed.
// Wind/outer-force physics has no renderer here; those proxy methods stay
// unregistered so the Lua boundary reports them instead of pretending to
// simulate.
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
    // removes the timeline once its blend reaches zero. The driver contract is
    // (label, time_ms, ease) — there is no separate fade flag.
    bool FadeInTimeline(const std::string& label, double duration_ms, double ease,
                        std::string& error);
    bool FadeOutTimeline(const std::string& label, double duration_ms, double ease,
                         std::string& error);
    // Direct blend set cancels a running fade (and its auto-stop). The driver
    // contract is (label, ratio, time_ms, ease, flags); a non-zero flags arms
    // the fade-out stop (the timeline is removed when the blend reaches zero).
    bool SetTimelineBlendRatio(const std::string& label, double ratio, std::string& error);
    bool SetTimelineBlendRatio(const std::string& label, double ratio, double transition_ms,
                               double ease, bool fade_out_stop, std::string& error);
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
    // Native SetVariableDiff drives a matched pair from a single control keyed
    // by the first label; GetVariableDiff reads that shared value back. The
    // two variables move as a difference: label receives +value, pair -value.
    bool SetVariableDiff(const std::string& label, const std::string& pair, double value,
                         double transition_ms, double ease, std::string& error);

    // ---- player transforms (applied to the container layer) ----
    // Artemis contract: setCoord(x, y, z, angle) is an instant placement
    // (z is depth and is not used by the 2D compositor); setScale(scale,
    // origin_x, origin_y) scales uniformly around a model-space pivot.
    void SetCoordAngle(double x, double y, double z, double angle);
    void SetScaleOrigin(double scale, double origin_x, double origin_y);
    // Engine-specific eased variants kept for the framework setters/tests.
    void SetCoord(double x, double y, double transition_ms, double ease);
    void SetScale(double sx, double sy, double transition_ms, double ease);
    void SetRot(double degrees, double transition_ms, double ease);
    // 0xRRGGBBAA: the game scripts build 0xRRGGBBff and the native driver
    // initializes to 0x808080FF (gray). E-mote tints use MODULATE2X, so 0x80
    // is the neutral channel (the renderer doubles and clamps); the reference
    // host documents the same for corner colors. Alpha is the low byte.
    void SetColor(uint32_t rrggbbaa, double transition_ms, double ease);
    uint32_t GetColor() const;
    // 0..1 luminance blend (native SetGrayscale(value, time, ease)); applied to
    // the textured scene layers at render time.
    void SetGrayscale(double value, double transition_ms, double ease);
    double GetGrayscale() const { return grayscale_.value; }
    // Engine knobs the game scripts set: mesh subdivision ratio and the
    // hair/bust sway amplitudes. Recorded (clamped) so scripts do not fall into
    // the unregistered stub; mesh subdivision and physics are not rendered yet.
    void SetMeshDivisionRatio(double ratio);
    double MeshDivisionRatio() const { return mesh_division_ratio_; }
    // Native createEmoteLayer width/height: the model's coordinate origin sits
    // at the centre of that box (reference host model_origin), before setCoord.
    void SetLayerSize(int width, int height) { layer_w_ = width; layer_h_ = height; }
    void SetHairScale(double scale);
    double HairScale() const { return hair_scale_; }
    void SetBustScale(double scale);
    double BustScale() const { return bust_scale_; }
    // metadata.selectorControl crossfade: the selected option gets onValue and
    // neighbours blend by min(|value-index|,1) (reference host formula).
    // Exposed for tests/diagnostics.
    static double SelectorOptionValue(const EmoteSelector& selector, double value, size_t index);
    void GetCoord(double* x, double* y) const;
    void GetScale(double* x, double* y) const;
    double GetRot() const;
    double GetOriginX() const { return origin_x_; }
    double GetOriginY() const { return origin_y_; }
    double GetZ() const { return z_; }
    void SetMirror(bool mirror);
    bool IsMirrored() const { return mirror_; }
    void Show();
    void Hide();
    bool IsHidden() const { return hidden_; }

    // ---- clock ----
    void Progress(double delta_ms);
    // Advance exactly one 60 fps frame. The driver's Step commits one internal
    // control step and re-evaluates the scene; here the whole player clock
    // advances one frame so scripts can drive frame-by-frame playback.
    void Step();
    bool IsAnimating() const;
    // Complete every transition instantly; non-loop timelines jump to their
    // final frame and hold it (looping timelines keep playing — documented
    // boundary, the original skip drives its own controller state).
    void Skip();
    // Advance the base motion clock to the model's sync frame (the reference
    // hosts treat it as the meaningful end of the base motion). No-op when the
    // model has no sync frame.
    void SkipToSync();
    // Complete transitions only, without jumping timeline positions.
    void Pass();

    // ---- rendering ----
    // SetProps on the bare container id (a texture-less holder layer the
    // compositor materializes) then evaluates the scene at the base frame.
    bool Render(Compositor& compositor, const std::string& id, std::string& error);
    // Point test over this player's installed shape layers (SDK `shape/...`
    // nodes). An empty label matches every shape; coordinates are stage pixels.
    bool Contains(Compositor& compositor, const std::string& id, const std::string& label,
                  double x, double y) const;
    // Per-frame tick: advance on the wall clock, then redraw. Returns false
    // when the scene rejected the frame; the player keeps ticking either way.
    // With auto progress disabled (createEmoteLayer progress=false, the
    // Windows/PS4/Switch script path) the clock only moves through explicit
    // Progress()/Step() calls.
    bool Update(double now_ms, Compositor* compositor, const std::string& id);
    void SetAutoProgress(bool on) { auto_progress_ = on; }
    bool AutoProgress() const { return auto_progress_; }
    void RemoveLayers(Compositor& compositor, const std::string& id);

private:
    struct Animated {
        double value = 0, start = 0, target = 0, duration_ms = 0, elapsed_ms = 0, weight = 1;
        Animated() = default;
        // The initial value is also the Finish() target: a skip/pass before
        // any Set must not snap the channel back to zero.
        explicit Animated(double v) : value(v), start(v), target(v) {}
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
    // metadata.eyeControl runtime: the native phase model is 40% closing, 20%
    // closed hold and 40% opening at 2.5x the nominal speed; waits never make
    // IsAnimating() true (a pending blink must not hold a scenario wait).
    struct BlinkRuntime {
        enum Phase { kIdle, kClosing, kClosedHold, kOpening };
        double wait = 0, frame = 0;
        Phase phase = kIdle;
        uint32_t rng = 1;
    };
    void ScheduleBlink(const EmoteBlink& blink, BlinkRuntime& state);
    void AdvanceBlinks(double dt_ms);
    void ApplyBlinks(std::map<std::string, double>& out) const;
    // metadata.selectorControl: the selector label's current value picks an
    // option; items crossfade by min(|value-index|,1) (reference host formula).
    // metadata.selectorControl: the selector label's current value picks an
    // option; items crossfade by min(|value-index|,1) (reference host formula).
    void ApplySelectors(std::map<std::string, double>& out) const;

    std::shared_ptr<const EmoteModel> model_;
    EmoteScene scene_;
    std::vector<std::pair<std::string, PlayingTimeline>> playing_;  // insertion order
    std::deque<std::pair<std::string, int>> queued_;                // sequential waits
    std::map<std::string, Animated> variables_;                     // model labels at 0
    std::vector<BlinkRuntime> blinks_;                              // parallel to model Blinks()
    Animated coord_x_, coord_y_, rot_;
    Animated scale_x_{1}, scale_y_{1}, alpha_{1}, grayscale_{0};
    uint32_t color_rgb_ = 0xFFFFFF;
    double base_frame_ = 0, last_now_ms_ = -1;
    // Native transform contract: translate(coord) * rotate(angle) *
    // scale * translate(-origin); origin/z are instant (non-animated).
    double z_ = 0, origin_x_ = 0, origin_y_ = 0;
    int layer_w_ = 0, layer_h_ = 0;
    double mesh_division_ratio_ = 1, hair_scale_ = 1, bust_scale_ = 1;
    bool auto_progress_ = true;
    bool mirror_ = false, hidden_ = false;
};
}
