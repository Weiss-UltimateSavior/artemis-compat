#include "core/engine_context.h"

// Link an actual consumer rather than only creating a static archive: this
// pulls the engine graph and all selected platform backends into a shared lib.
extern "C" bool artemis_embedded_probe() {
    artc::EngineContext engine;
    const bool rejected = !engine.Start(false);
    engine.Shutdown();
    engine.Shutdown();
    return rejected && !engine.Opened() && !engine.Started();
}
