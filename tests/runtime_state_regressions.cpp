// runtime_state_regressions.cpp — load-order contract.

#include "script/runtime_state.h"

#include <iostream>
#include <string>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
using artc::LoadPhase;
} // namespace

int main() {
    Check(artc::ValidLoadTransition(LoadPhase::None, LoadPhase::ResetEphemeral), "none -> reset");
    Check(artc::ValidLoadTransition(LoadPhase::RestoreData, LoadPhase::SnapshotScene), "data -> snapshot");
    Check(artc::ValidLoadTransition(LoadPhase::SnapshotScene, LoadPhase::RebuildDerived),
          "snapshot -> rebuild (B after A)");
    Check(!artc::ValidLoadTransition(LoadPhase::RebuildDerived, LoadPhase::SnapshotScene),
          "rebuild before snapshot is rejected");
    Check(!artc::ValidLoadTransition(LoadPhase::RestoreData, LoadPhase::ResetEphemeral),
          "backwards transition rejected");
    Check(std::string(artc::LoadPhaseName(LoadPhase::RebuildDerived)) == "rebuild-derived", "phase name");
    if (g_failures == 0) std::cout << "runtime_state_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
