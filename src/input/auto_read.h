#pragma once
#include <algorithm>

namespace artc {

// The reading interval starts after text and synchronized voices finish.
// Use the engine's paused clock, and preserve elapsed reading time when a
// menu suspends the scenario instead of counting time spent in that menu.
class AutoReadTimer {
public:
    bool Ready(double now, double delay, bool blocked) {
        if (blocked) { Reset(); return false; }
        if (since_ < 0) since_ = now;
        return now - since_ >= std::max(0.0, delay);
    }
    void Reset() { since_ = -1; }
    double Elapsed(double now) const { return since_ < 0 ? -1 : now - since_; }
    void Restore(double now, double elapsed) { since_ = elapsed < 0 ? -1 : now - elapsed; }
private:
    double since_ = -1;
};

} // namespace artc
