#pragma once
#include "script/lua_engine.h"

namespace artc {
// Engine-thread state for a live session. BeginFrame(false) requires a drawable,
// current context because dragout may run script that mutates GL-backed layers.
// BeginFrame(true) never runs script. True context loss must stay paused.
class SessionPause {
public:
    // Return true before any old-VM callback when a drawable frame will reset.
    bool BeginFrame(LuaEngine& lua, bool paused) {
        if (!paused && lua.ConsumeResetRequest()) {
            Reset();
            return true;
        }
        Apply(lua, paused);
        return false;
    }
    // The caller is replacing the Lua session, so old input belongs to no one.
    void Reset() { applied_ = -1; cancel_drag_ = false; }
private:
    void Apply(LuaEngine& lua, bool paused) {
        if (applied_ == static_cast<int>(paused)) return;
        applied_ = paused;
        if (paused) {
            lua.PauseClock();
            for (int key = 0; key < 256; ++key) lua.PushKeyUp(key);
            lua.SetTouchCount(0);
            lua.EndFrame(); // Drop release edges, not a delayed click.
            cancel_drag_ = true;
        } else {
            lua.ResumeClock();
            if (cancel_drag_) {
                cancel_drag_ = false; // Consume before invoking user script.
                lua.CancelDrag();
            }
        }
    }
    int applied_ = -1;
    bool cancel_drag_ = false;
};
} // namespace artc
