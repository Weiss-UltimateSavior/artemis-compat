#include "render/emote_player.h"
#include "render/compositor.h"
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
        value = v;
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
    *this = std::move(next);
    error.clear();
    return true;
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

bool EmotePlayer::FadeInTimeline(const std::string& label, double duration_ms, int flags,
                                 std::string& error) {
    auto* entry = EnsurePlaying(label, flags, 0.0, error);
    if (!entry) return false;
    entry->blend.Set(1.0, duration_ms, 0);
    error.clear();
    return true;
}

bool EmotePlayer::FadeOutTimeline(const std::string& label, double duration_ms,
                                  std::string& error) {
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second.blend.Set(0.0, duration_ms, 0);
        entry.second.fade_out_stop = true;
        error.clear();
        return true;
    }
    error = "E-mote timeline not playing: " + label;
    return false;
}

bool EmotePlayer::SetTimelineBlendRatio(const std::string& label, double ratio,
                                        std::string& error) {
    for (auto& entry : playing_) {
        if (entry.first != label) continue;
        entry.second.blend.Set(std::clamp(ratio, 0.0, 1.0), 0, 0);
        entry.second.fade_out_stop = false;
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

void EmotePlayer::SetCoord(double x, double y, double transition_ms, double ease) {
    coord_x_.Set(x, transition_ms, ease);
    coord_y_.Set(y, transition_ms, ease);
}

void EmotePlayer::SetScale(double sx, double sy, double transition_ms, double ease) {
    scale_x_.Set(sx, transition_ms, ease);
    scale_y_.Set(sy, transition_ms, ease);
}

void EmotePlayer::SetRot(double degrees, double transition_ms, double ease) {
    rot_.Set(degrees, transition_ms, ease);
}

void EmotePlayer::SetColor(uint32_t aarrggbb, double transition_ms, double ease) {
    color_rgb_ = aarrggbb & 0xFFFFFFu;
    alpha_.Set((aarrggbb >> 24) / 255.0, transition_ms, ease);
}

uint32_t EmotePlayer::GetColor() const {
    const int a = std::clamp(static_cast<int>(std::lround(alpha_.value * 255)), 0, 255);
    return (static_cast<uint32_t>(a) << 24) | color_rgb_;
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

void EmotePlayer::Progress(double delta_ms) {
    if (!model_) return;
    if (!(delta_ms > 0)) delta_ms = 0;
    if (delta_ms > kMaxProgressMs) delta_ms = kMaxProgressMs;  // original Update clamp
    const double frames = delta_ms * kFramesPerMillisecond;
    base_frame_ += frames;  // idle base motion keeps breathing alive
    coord_x_.Advance(delta_ms);
    coord_y_.Advance(delta_ms);
    rot_.Advance(delta_ms);
    scale_x_.Advance(delta_ms);
    scale_y_.Advance(delta_ms);
    alpha_.Advance(delta_ms);
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
        scale_x_.animating() || scale_y_.animating() || alpha_.animating())
        return true;
    for (const auto& v : variables_)
        if (v.second.animating()) return true;
    for (const auto& e : playing_)
        if (e.second.blend.animating() || !e.second.finished) return true;
    return false;
}

void EmotePlayer::Skip() {
    coord_x_.Finish();
    coord_y_.Finish();
    rot_.Finish();
    scale_x_.Finish();
    scale_y_.Finish();
    alpha_.Finish();
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
    std::map<std::string, std::string> props{{"left", Num(coord_x_.value)},
                                             {"top", Num(coord_y_.value)},
                                             {"rotate", Num(rot_.value)},
                                             {"xscale", Num(scale_x_.value * 100)},
                                             {"yscale", Num(scale_y_.value * 100)},
                                             {"alpha", Num(std::lround(alpha_.value * 255))},
                                             {"visible", hidden_ ? "0" : "1"},
                                             {"reversex", mirror_ ? "1" : "0"}};
    compositor.SetProps(id, props);
    return scene_.Render(compositor, id, base_frame_, ComposeVariables(), error);
}

bool EmotePlayer::Update(double now_ms, Compositor* compositor, const std::string& id) {
    double dt = 0;
    if (last_now_ms_ >= 0) {
        dt = now_ms - last_now_ms_;
        if (!(dt > 0)) dt = 0;  // paused frame or clock reset
    }
    last_now_ms_ = now_ms;
    Progress(dt);
    if (!compositor) return true;
    std::string error;
    if (!Render(*compositor, id, error)) return false;
    return true;
}

void EmotePlayer::RemoveLayers(Compositor& compositor, const std::string& id) {
    scene_.Remove(compositor, id);
}
}
