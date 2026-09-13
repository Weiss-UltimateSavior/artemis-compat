// runtime_state.h — load/save state buckets and the read-order contract.
//
// Every piece of runtime state belongs to exactly one bucket:
//   [A] Serializable   pure data persisted in the slot/global bank (variables,
//                      script cursor, call stack, scene layer props).
//   [B] RebuiltOnLoad  derived state recreated from A after the snapshot is
//                      restored (message pages, audio replay, scenario cursor,
//                      layer event registrations). Must NOT be restored before
//                      the snapshot or it is overwritten by the replay.
//   [C] Ephemeral      dropped on load (clicks, drags, active tweens/trans).
//
// The contract is enforced as a monotone phase order so a future refactor that
// applies B before the scene snapshot is caught instead of silently losing the
// restored page/wait.
#pragma once
#include <cstdint>

namespace artc {

enum class LoadPhase : uint8_t {
    None = 0,        // before anything
    ResetEphemeral,  // [C] dropped
    RestoreData,     // [A] variables / cursor
    SnapshotScene,   // [A] scene layer replay
    RebuildDerived,  // [B] onLoad reconstructs derived state
};

inline const char *LoadPhaseName(LoadPhase p) {
    switch (p) {
    case LoadPhase::None: return "none";
    case LoadPhase::ResetEphemeral: return "reset-ephemeral";
    case LoadPhase::RestoreData: return "restore-data";
    case LoadPhase::SnapshotScene: return "snapshot-scene";
    case LoadPhase::RebuildDerived: return "rebuild-derived";
    }
    return "?";
}

// Phases must be non-decreasing (never go backwards).
inline bool ValidLoadTransition(LoadPhase from, LoadPhase to) {
    return static_cast<uint8_t>(to) >= static_cast<uint8_t>(from);
}

} // namespace artc
