#include "render/emote_player.h"
#include "render/compositor.h"
#include "log/logger.h"
#include <algorithm>
#include <cmath>

namespace artc {
namespace {
double EaseWeight(double ease) {
    // Same exponent EmoteModel::Sample applies to keyframe easing.
    return ease >= 0 ? ease + 1 : 1 / (1 - ease);
}
std::string Num(double v) {
    return std::to_string(v);
}
}  // namespace

void EmotePlayer::Animated::Set(double v, double transition_ms, double ease) {
    if (!(transition_ms > 0)) {
        // Immediate sets update the target too: Skip()/Pass() call Finish()
        // (value = target) and must not undo an instant placement.
        value = v;
        start = v;
        target = v;
        duration_ms = 0;
        elapsed_ms = 0;
        return;
    }
    start = value;
    target = v;
    duration_ms = transition_ms;
    elapsed_ms = 0;
    weight = EaseWeight(ease);
}

void EmotePlayer::Animated::Advance(double dt_ms) {
    if (duration_ms <= 0) return;
    elapsed_ms += dt_ms;
    if (elapsed_ms >= duration_ms) {
        Finish();
        return;
    }
    const double ratio = std::clamp(elapsed_ms / duration_ms, 0.0, 1.0);
    value = start + (target - start) * std::pow(ratio, weight);
}

void EmotePlayer::Animated::Finish() {
    value = target;
    duration_ms = 0;
    elapsed_ms = 0;
}

bool EmotePlayer::Load(std::shared_ptr<const EmoteModel> model, std::string& error) {
    if (!model) {
        error = "missing E-mote model";
        return false;
    }
    EmotePlayer next;  // a failed Load must not leave a half-swapped player
    if (!next.scene_.Load(model, error)) return false;
    next.model_ = std::move(model);
    for (const auto& label : next.model_->Variables()) next.variables_[label] = Animated();
    // Eye blinks run on a deterministic per-player LCG (the shipped models only
    // need a repeatable interval distribution; tests pin min==max).
    const auto& blinks = next.model_->Blinks();
    next.blinks_.resize(blinks.size());
    for (size_t i = 0; i < blinks.size(); ++i) {
        next.blinks_[i].rng = 0x9E3779B9u * static_cast<uint32_t>(i + 1) | 1u;
        next.blinks_[i].frame = blinks[i].begin;
        next.ScheduleBlink(blinks[i], next.blinks_[i]);
    }
    // metadata.mirror flips the model by default; Lua setMirror can override.
    next.mirror_ = next.model_->Mirrored();
    *this = std::move(next);
    error.clear();
    return true;
}

void EmotePlayer::ScheduleBlink(const EmoteBlink& blink, BlinkRuntime& state) {
    state.rng = state.rng * 1664525u + 1013904223u;
    const double unit = static_cast<double>(state.rng) / 4294967295.0;
    const double min = std::max(0.0, blink.interval_min);
    const double max = std::max(min, blink.interval_max);
    state.wait = min + (max - min) * unit;
}

void EmotePlayer::AdvanceBlinks(double dt_ms) {
    if (!model_) return;
    double frames = dt_ms * kFramesPerMillisecond;
    if (!(frames > 0)) return;
    const auto& blinks = model_->Blinks();
    for (size_t i = 0; i < blinks.size() && i < blinks_.size(); ++i) {
        const EmoteBlink& blink = blinks[i];
        BlinkRuntime& state = blinks_[i];
        if (!blink.enabled || !blink.blink_enabled) continue;
        // Long host steps continue across phase boundaries so the eye can not
        // stay stuck half closed.
        for (int step = 0; step < 8 && frames > 0; ++step) {
            switch (state.phase) {
            case BlinkRuntime::kIdle:
                if (frames < state.wait) {
                    state.wait -= frames;
                    frames = 0;
                } else {
                    frames -= state.wait;
                    state.wait = 0;
                    state.phase = BlinkRuntime::kClosing;
                }
                break;
            case BlinkRuntime::kClosing: {
                const double span = std::max(0.0, blink.end - blink.begin);
                const double speed = span * 2.5 / std::max(1.0, blink.frames);
                const double remaining = speed > 0 ? (blink.end - state.frame) / speed : 0;
                if (frames < remaining && remaining - frames > 1.0e-4) {
                    state.frame += speed * frames;
                    frames = 0;
                } else {
                    state.frame = blink.end;
                    frames -= remaining;
                    state.wait = blink.frames / 5.0;
                    state.phase = BlinkRuntime::kClosedHold;
                }
                break;
            }
            case BlinkRuntime::kClosedHold:
                if (frames < state.wait) {
                    state.wait -= frames;
                    frames = 0;
                } else {
                    frames -= state.wait;
                    state.wait = 0;
                    state.phase = BlinkRuntime::kOpening;
                }
                break;
            case BlinkRuntime::kOpening: {
                const double span = std::max(0.0, blink.end - blink.begin);
                const double speed = span * 2.5 / std::max(1.0, blink.frames);
                const double remaining = speed > 0 ? (state.frame - blink.begin) / speed : 0;
                if (frames < remaining && remaining - frames > 1.0e-4) {
                    state.frame -= speed * frames;
                    frames = 0;
                } else {
                    state.frame = blink.begin;
                    frames -= remaining;
                    state.phase = BlinkRuntime::kIdle;
                    ScheduleBlink(blink, state);
                }
                break;
            }
            }
        }
    }
}

void EmotePlayer::ApplyBlinks(std::map<std::string, double>& out) const {
    if (!model_) return;
    const auto& blinks = model_->Blinks();
    for (size_t i = 0; i < blinks.size() && i < blinks_.size(); ++i) {
        const EmoteBlink& blink = blinks[i];
        const BlinkRuntime& state = blinks_[i];
        if (state.phase == BlinkRuntime::kIdle) continue;
        const auto it = out.find(blink.variable);
        const double base = it == out.end() ? 0 : it->second;
        const double span = blink.end - blink.begin;
        if (base < std::min(blink.begin, blink.end) ||
            base > std::max(blink.begin, blink.end) || std::abs(span) <= 1e-9)
            continue;
        const double amount = std::clamp((state.frame - blink.begin) / span, 0.0, 1.0);
        out[blink.variable] = base + (blink.end - base) * amount;
    }
}

double EmotePlayer::SelectorOptionValue(const EmoteSelector& selector, double value, size_t index) {
    if (selector.items.empty() || index >= selector.items.size()) return 0;
    const double clamped = std::clamp(value, 0.0, static_cast<double>(selector.items.size() - 1));
    const double distance = std::min(std::abs(clamped - static_cast<double>(index)), 1.0);
    const auto& item = selector.items[index];
    return item.on + (item.off - item.on) * distance;
}

void EmotePlayer::ApplySelectors(std::map<std::string, double>& out) const {
    if (!model_) return;
    for (const auto& selector : model_->Selectors()) {
        if (!selector.enabled || selector.items.empty()) continue;
        const auto it = out.find(selector.label);
        const double value = it == out.end() ? 0.0 : it->second;
        for (size_t i = 0; i < selector.items.size(); ++i)
            out[selector.items[i].label] = SelectorOptionValue(selector, value, i);
    }
}

const EmoteTimeline* EmotePlayer::Timeline(const std::string& label) const {
    if (!model_) return nullptr;
    const auto& timelines = model_->Timelines();
    const auto it = timelines.find(label);
    return it == timelines.end() ? nullptr : &it->second;
}

bool EmotePlayer::Looping(const EmoteTimeline& timeline, bool hold_end) const {
    if (hold_end) return false;
    // Mirrors EmoteModel::Sample: an explicit loop range wraps, and a missing
    // last_time plays forever.
    return (timeline.loop_end > timeline.loop_begin && timeline.loop_begin >= 0) ||
           timeline.last_time < 0;
}

EmotePlayer::PlayingTimeline* EmotePlayer::EnsurePlaying(const std::string& label, int flags,
                                                         double blend, std::string& error) {
    if (!model_) {
        error = "E-mote player has no model";
        return nullptr;
    }
    if (!Timeline(label)) {
        error = "unknown E-mote timeline: " + label;
        return nullptr;
    }
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second = PlayingTimeline();
        entry.second.flags = flags;
        entry.second.blend = Animated(blend);
        return &entry.second;
    }
    playing_.push_back({label, PlayingTimeline()});
    auto& entry = playing_.back().second;
    entry.flags = flags;
    entry.blend = Animated(blend);
    return &entry;
}

void EmotePlayer::StartQueued() {
    while (!queued_.empty()) {
        const bool busy = std::any_of(playing_.begin(), playing_.end(),
                                      [](const std::pair<std::string, PlayingTimeline>& e) {
                                          return !e.second.finished;
                                      });
        if (busy) return;
        const auto next = queued_.front();
        queued_.pop_front();
        std::string error;
        if (!EnsurePlaying(next.first, next.second, 1.0, error)) return;  // drop stale entry
    }
}

bool EmotePlayer::PlayTimeline(const std::string& label, int flags, std::string& error) {
    if (!model_) {
        error = "E-mote player has no model";
        return false;
    }
    for (auto it = queued_.begin(); it != queued_.end();) {
        if (it->first == label)
            it = queued_.erase(it);  // a restart supersedes its queued copy
        else
            ++it;
    }
    const bool busy = !queued_.empty() || std::any_of(playing_.begin(), playing_.end(),
                                                      [](const std::pair<std::string, PlayingTimeline>& e) {
                                                          return !e.second.finished;
                                                      });
    if ((flags & kTimelineSequential) != 0 && busy) {
        queued_.push_back({label, flags});
        error.clear();
        return true;
    }
    return EnsurePlaying(label, flags, 1.0, error) != nullptr;
}

void EmotePlayer::StopTimeline(const std::string& label) {
    for (auto it = playing_.begin(); it != playing_.end();) {
        if (it->first == label)
            it = playing_.erase(it);
        else
            ++it;
    }
    for (auto it = queued_.begin(); it != queued_.end();) {
        if (it->first == label)
            it = queued_.erase(it);
        else
            ++it;
    }
    StartQueued();
}

void EmotePlayer::StopAllTimelines() {
    playing_.clear();
    queued_.clear();
}

bool EmotePlayer::IsTimelinePlaying(const std::string& label) const {
    for (const auto& entry : playing_)
        if (entry.first == label) return !entry.second.finished;
    return false;
}

bool EmotePlayer::IsLoopTimeline(const std::string& label) const {
    const auto* timeline = Timeline(label);
    return timeline && Looping(*timeline, false);
}

double EmotePlayer::TimelineTotalFrames(const std::string& label) const {
    const auto* timeline = Timeline(label);
    if (!timeline) return -1;
    return timeline->last_time >= 0 ? timeline->last_time : timeline->loop_end;
}

bool EmotePlayer::FadeInTimeline(const std::string& label, double duration_ms, double ease,
                                 std::string& error) {
    auto* entry = EnsurePlaying(label, kTimelineParallel, 0.0, error);
    if (!entry) return false;
    entry->blend.Set(1.0, duration_ms, ease);
    error.clear();
    return true;
}

bool EmotePlayer::FadeOutTimeline(const std::string& label, double duration_ms, double ease,
                                  std::string& error) {
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second.blend.Set(0.0, duration_ms, ease);
        entry.second.fade_out_stop = true;
        error.clear();
        return true;
    }
    error = "E-mote timeline not playing: " + label;
    return false;
}

bool EmotePlayer::SetTimelineBlendRatio(const std::string& label, double ratio,
                                        std::string& error) {
    return SetTimelineBlendRatio(label, ratio, 0, 0, false, error);
}

bool EmotePlayer::SetTimelineBlendRatio(const std::string& label, double ratio,
                                        double transition_ms, double ease, bool fade_out_stop,
                                        std::string& error) {
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second.blend.Set(std::clamp(ratio, 0.0, 1.0), transition_ms, ease);
        entry.second.fade_out_stop = fade_out_stop;
        error.clear();
        return true;
    }
    error = "E-mote timeline not playing: " + label;
    return false;
}

double EmotePlayer::TimelineBlendRatio(const std::string& label, bool* found) const {
    for (const auto& entry : playing_) {
        if (entry.first != label) continue;
        if (found) *found = true;
        return entry.second.blend.value;
    }
    if (found) *found = false;
    return 0;
}

bool EmotePlayer::SetTimelineHoldEnd(const std::string& label, bool loop, std::string& error) {
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second.hold_end = !loop;
        error.clear();
        return true;
    }
    error = "E-mote timeline not playing: " + label;
    return false;
}

int EmotePlayer::CountMainTimelines() const {
    if (!model_) return 0;
    int count = 0;
    for (const auto& t : model_->Timelines())
        if (!t.second.difference) ++count;
    return count;
}

std::string EmotePlayer::MainTimelineLabelAt(int index) const {
    if (!model_ || index < 0) return {};
    int i = 0;
    for (const auto& t : model_->Timelines())
        if (!t.second.difference && i++ == index) return t.first;
    return {};
}

int EmotePlayer::CountDiffTimelines() const {
    if (!model_) return 0;
    int count = 0;
    for (const auto& t : model_->Timelines())
        if (t.second.difference) ++count;
    return count;
}

std::string EmotePlayer::DiffTimelineLabelAt(int index) const {
    if (!model_ || index < 0) return {};
    int i = 0;
    for (const auto& t : model_->Timelines())
        if (t.second.difference && i++ == index) return t.first;
    return {};
}

int EmotePlayer::CountPlayingTimelines() const { return static_cast<int>(playing_.size()); }

std::string EmotePlayer::PlayingTimelineLabelAt(int index) const {
    if (index < 0 || index >= static_cast<int>(playing_.size())) return {};
    return playing_[index].first;
}

int EmotePlayer::PlayingTimelineFlagsAt(int index) const {
    if (index < 0 || index >= static_cast<int>(playing_.size())) return 0;
    return playing_[index].second.flags;
}

int EmotePlayer::CountVariables() const {
    if (!model_) return 0;
    return static_cast<int>(model_->Variables().size());
}

std::string EmotePlayer::VariableLabelAt(int index) const {
    if (!model_ || index < 0) return {};
    int i = 0;
    for (const auto& label : model_->Variables())
        if (i++ == index) return label;
    return {};
}

bool EmotePlayer::SetVariable(const std::string& label, double value, double transition_ms,
                              double ease, std::string& error) {
    const auto it = variables_.find(label);
    if (it == variables_.end()) {
        error = "unknown E-mote variable: " + label;
        return false;
    }
    it->second.Set(value, transition_ms, ease);
    error.clear();
    return true;
}

double EmotePlayer::GetVariable(const std::string& label, bool* found) const {
    const auto it = variables_.find(label);
    if (it == variables_.end()) {
        if (found) *found = false;
        return 0;
    }
    if (found) *found = true;
    return it->second.value;
}

bool EmotePlayer::SetVariableDiff(const std::string& label, const std::string& pair,
                                  double value, double transition_ms, double ease,
                                  std::string& error) {
    if (!variables_.count(label)) {
        error = "unknown E-mote variable: " + label;
        return false;
    }
    if (!variables_.count(pair)) {
        error = "unknown E-mote variable: " + pair;
        return false;
    }
    variables_[label].Set(value, transition_ms, ease);
    variables_[pair].Set(-value, transition_ms, ease);  // difference pair
    error.clear();
    return true;
}

void EmotePlayer::SetCoord(double x, double y, double transition_ms, double ease) {
    coord_x_.Set(x, transition_ms, ease);
    coord_y_.Set(y, transition_ms, ease);
}

void EmotePlayer::SetCoordAngle(double x, double y, double z, double angle) {
    // Artemis setCoord(x, y, z, angle): an instant placement; z is depth and
    // the compositor has no 3D stage.
    z_ = z;
    coord_x_.Set(x, 0, 0);
    coord_y_.Set(y, 0, 0);
    rot_.Set(angle, 0, 0);
}

void EmotePlayer::SetScaleOrigin(double scale, double origin_x, double origin_y) {
    // Artemis setScale(scale, origin_x, origin_y): uniform scale around a
    // model-space pivot (folded into the container position at render time).
    origin_x_ = origin_x;
    origin_y_ = origin_y;
    scale_x_.Set(scale, 0, 0);
    scale_y_.Set(scale, 0, 0);
}

void EmotePlayer::SetScale(double sx, double sy, double transition_ms, double ease) {
    origin_x_ = 0;
    origin_y_ = 0;
    scale_x_.Set(sx, transition_ms, ease);
    scale_y_.Set(sy, transition_ms, ease);
}

void EmotePlayer::SetRot(double degrees, double transition_ms, double ease) {
    rot_.Set(degrees, transition_ms, ease);
}

void EmotePlayer::SetColor(uint32_t rrggbbaa, double transition_ms, double ease) {
    // Artemis scripts build 0xRRGGBBff; the native driver's neutral is
    // 0x808080FF (gray, opaque), so alpha is the low byte.
    color_rgb_ = (rrggbbaa >> 8) & 0xFFFFFFu;
    alpha_.Set((rrggbbaa & 0xFFu) / 255.0, transition_ms, ease);
}

uint32_t EmotePlayer::GetColor() const {
    const int a = std::clamp(static_cast<int>(std::lround(alpha_.value * 255)), 0, 255);
    return (color_rgb_ << 8) | static_cast<uint32_t>(a);
}

void EmotePlayer::SetGrayscale(double value, double transition_ms, double ease) {
    grayscale_.Set(std::clamp(value, 0.0, 1.0), transition_ms, ease);
}

void EmotePlayer::SetMeshDivisionRatio(double ratio) {
    if (!(ratio > 0) || !std::isfinite(ratio)) return;
    mesh_division_ratio_ = std::clamp(ratio, 0.05, 1.0);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log(kLogInfo, "emote: mesh division ratio accepted; subdivision is not rendered yet");
    }
}

void EmotePlayer::SetHairScale(double scale) {
    if (!std::isfinite(scale)) return;
    hair_scale_ = std::max(0.0, scale);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log(kLogInfo, "emote: hair sway scale recorded; physics is not simulated");
    }
}

void EmotePlayer::SetBustScale(double scale) {
    if (!std::isfinite(scale)) return;
    bust_scale_ = std::max(0.0, scale);
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log(kLogInfo, "emote: bust sway scale recorded; physics is not simulated");
    }
}

void EmotePlayer::GetCoord(double* x, double* y) const {
    if (x) *x = coord_x_.value;
    if (y) *y = coord_y_.value;
}

void EmotePlayer::GetScale(double* x, double* y) const {
    if (x) *x = scale_x_.value;
    if (y) *y = scale_y_.value;
}

double EmotePlayer::GetRot() const { return rot_.value; }

void EmotePlayer::SetMirror(bool mirror) { mirror_ = mirror; }

void EmotePlayer::Show() { hidden_ = false; }

void EmotePlayer::Hide() { hidden_ = true; }

void EmotePlayer::Step() {
    // One 60 fps frame; kFramesPerMillisecond maps frames 1:1, so the ms value
    // is the frame duration in milliseconds.
    Progress(1.0 / kFramesPerMillisecond);
}

void EmotePlayer::Progress(double delta_ms) {
    if (!model_) return;
    if (!(delta_ms > 0)) delta_ms = 0;
    if (delta_ms > kMaxProgressMs) delta_ms = kMaxProgressMs;  // original Update clamp
    const double frames = delta_ms * kFramesPerMillisecond;
    base_frame_ += frames;  // idle base motion keeps breathing alive
    AdvanceBlinks(delta_ms);
    coord_x_.Advance(delta_ms);
    coord_y_.Advance(delta_ms);
    rot_.Advance(delta_ms);
    scale_x_.Advance(delta_ms);
    scale_y_.Advance(delta_ms);
    alpha_.Advance(delta_ms);
    grayscale_.Advance(delta_ms);
    for (auto& v : variables_) v.second.Advance(delta_ms);
    bool freed = false;  // a slot opened for the sequential queue
    for (auto it = playing_.begin(); it != playing_.end();) {
        auto& entry = it->second;
        entry.blend.Advance(delta_ms);
        if (entry.fade_out_stop && !entry.blend.animating() && entry.blend.value <= 0) {
            it = playing_.erase(it);
            freed = true;
            continue;
        }
        if (!entry.finished) {
            entry.position += frames;
            const auto* timeline = Timeline(it->first);
            if (timeline) {
                if (Looping(*timeline, entry.hold_end)) {
                    const double begin = timeline->loop_begin, end = timeline->loop_end;
                    if (end > begin && begin >= 0 && entry.position >= end)
                        entry.position = begin + std::fmod(entry.position - begin, end - begin);
                } else if (timeline->last_time >= 0 && entry.position >= timeline->last_time) {
                    entry.position = timeline->last_time;  // HOLD the final pose
                    entry.finished = true;
                    freed = true;
                } else if (entry.hold_end && timeline->loop_end > 0 &&
                           entry.position >= timeline->loop_end) {
                    entry.position = timeline->loop_end;
                    entry.finished = true;
                    freed = true;
                }
            }
        }
        ++it;
    }
    if (freed) StartQueued();
}

bool EmotePlayer::IsAnimating() const {
    if (coord_x_.animating() || coord_y_.animating() || rot_.animating() ||
        scale_x_.animating() || scale_y_.animating() || alpha_.animating() ||
        grayscale_.animating())
        return true;
    for (const auto& v : variables_)
        if (v.second.animating()) return true;
    for (const auto& e : playing_)
        if (e.second.blend.animating() || !e.second.finished) return true;
    return false;
}

void EmotePlayer::SkipToSync() {
    if (!model_) return;
    const auto& base = model_->Document().root.At("metadata").At("base");
    const double sync = model_->SyncFrame(base.At("chara").string, base.At("motion").string);
    if (sync >= 0) base_frame_ = sync;
}

void EmotePlayer::Skip() {
    coord_x_.Finish();
    coord_y_.Finish();
    rot_.Finish();
    scale_x_.Finish();
    scale_y_.Finish();
    alpha_.Finish();
    grayscale_.Finish();
    for (auto& v : variables_) v.second.Finish();
    bool freed = false;
    for (auto it = playing_.begin(); it != playing_.end();) {
        auto& entry = it->second;
        entry.blend.Finish();
        if (entry.fade_out_stop && entry.blend.value <= 0) {
            it = playing_.erase(it);
            freed = true;
            continue;
        }
        const auto* timeline = Timeline(it->first);
        if (timeline) {
            if (!Looping(*timeline, entry.hold_end) && timeline->last_time >= 0) {
                entry.position = timeline->last_time;
                entry.finished = true;
                freed = true;
            } else if (entry.hold_end && timeline->loop_end > 0) {
                entry.position = timeline->loop_end;
                entry.finished = true;
                freed = true;
            }
        }
        ++it;
    }
    if (freed) StartQueued();
}

void EmotePlayer::Pass() {
    coord_x_.Finish();
    coord_y_.Finish();
    rot_.Finish();
    scale_x_.Finish();
    scale_y_.Finish();
    alpha_.Finish();
    grayscale_.Finish();
    for (auto& v : variables_) v.second.Finish();
    for (auto& e : playing_) e.second.blend.Finish();
}

std::map<std::string, double> EmotePlayer::ComposeVariables() const {
    std::map<std::string, double> out;
    for (const auto& v : variables_) out[v.first] = v.second.value;
    if (!model_) return out;
    std::map<std::string, double> sampled;  // Sample REPLACES its out map
    for (const auto& entry : playing_) {
        const auto* timeline = Timeline(entry.first);
        if (!timeline || entry.second.blend.value <= 0) continue;
        if (!model_->Sample(entry.first, entry.second.position, sampled)) continue;
        const double blend = entry.second.blend.value;
        for (const auto& var : sampled) {
            const auto base = out.find(var.first);
            if (timeline->difference)
                out[var.first] = (base == out.end() ? 0 : base->second) + var.second * blend;
            else
                out[var.first] = base == out.end()
                                     ? var.second * blend
                                     : base->second + (var.second - base->second) * blend;
        }
    }
    // Eye blinks apply last, against whatever the timelines and script values
    // produced (the reference host order).
    ApplyBlinks(out);
    // Selector controls resolve after the variable map is merged, overriding
    // any timeline writes to their option variables.
    ApplySelectors(out);
    return out;
}

bool EmotePlayer::Render(Compositor& compositor, const std::string& id, std::string& error) {
    if (!model_) {
        error = "E-mote player has no model";
        return false;
    }
    if (id.empty()) {
        error = "missing E-mote layer id";
        return false;
    }
    // The bare id materializes a texture-less holder layer; scene children are
    // keyed id.<node> and inherit this transform through the dotted-id chain.
    // Native order is translate(coord) * rotate * scale * translate(-origin):
    // the pivot shift is folded into the container position.
    const double radians = rot_.value * 3.14159265358979323846 / 180.0;
    const double cosr = std::cos(radians), sinr = std::sin(radians);
    const double pivot_x = scale_x_.value * cosr * origin_x_ - scale_y_.value * sinr * origin_y_;
    const double pivot_y = scale_x_.value * sinr * origin_x_ + scale_y_.value * cosr * origin_y_;
    // createEmoteLayer's width/height box centres the model origin; setCoord
    // then moves the model inside that box (native model_origin transform).
    const double center_x = layer_w_ * 0.5, center_y = layer_h_ * 0.5;
    std::map<std::string, std::string> props{{"left", Num(center_x + coord_x_.value - pivot_x)},
                                             {"top", Num(center_y + coord_y_.value - pivot_y)},
                                             {"rotate", Num(rot_.value)},
                                             {"xscale", Num(scale_x_.value * 100)},
                                             {"yscale", Num(scale_y_.value * 100)},
                                             {"alpha", Num(std::lround(alpha_.value * 255))},
                                             {"visible", hidden_ ? "0" : "1"},
                                             {"reversex", mirror_ ? "1" : "0"}};
    compositor.SetProps(id, props);
    // E-mote SetColor tints are MODULATE2X: 0x80 is the neutral channel.
    const auto doubled=[&](int shift) {
        return std::min(0xFFu,((color_rgb_>>shift)&0xFFu)*2u);
    };
    const uint32_t multiply=(doubled(16)<<16)|(doubled(8)<<8)|doubled(0);
    return scene_.Render(compositor, id, base_frame_, ComposeVariables(), error,
                         grayscale_.value, multiply, mesh_division_ratio_);
}

bool EmotePlayer::Update(double now_ms, Compositor* compositor, const std::string& id) {
    double dt = 0;
    if (last_now_ms_ >= 0) {
        dt = now_ms - last_now_ms_;
        if (!(dt > 0)) dt = 0;  // paused frame or clock reset
    }
    last_now_ms_ = now_ms;
    // progress=false layers are driven by explicit progress()/step() calls.
    if (auto_progress_) Progress(dt);
    if (!compositor) return true;
    std::string error;
    if (!Render(*compositor, id, error)) return false;
    return true;
}

bool EmotePlayer::Contains(Compositor& compositor, const std::string& id,
                           const std::string& label, double x, double y) const {
    return scene_.HitTest(compositor, id, label, x, y);
}

void EmotePlayer::RemoveLayers(Compositor& compositor, const std::string& id) {
    scene_.Remove(compositor, id);
}
}
