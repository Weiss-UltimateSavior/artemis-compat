#pragma once

namespace artc {
// UI callers publish intent; only the engine worker touches its object graph.
void RequestAudioPause(bool paused);
}
