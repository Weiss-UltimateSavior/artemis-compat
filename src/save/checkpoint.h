#pragma once

#include "save/native_save.h"
#include "util/save_storage.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace artc {

struct CheckpointWait {
    bool waiting = false, timed = false, accept_input = false;
    bool announced = false, sound = false, transition = false;
    std::string sound_key, video_key;
    uint64_t remaining_ms = 0;
    double auto_elapsed = -1;
};

struct CheckpointFrame {
    std::string file;
    size_t pc = 0;
    bool halted = false, macro_scope = false;
    CheckpointWait wait;
};

struct CheckpointFlow {
    std::string current_file;
    size_t pc = 0;
    bool loaded = false, halted = false, pc_pending = false;
    std::vector<CheckpointFrame> frames;
    std::map<std::string, std::string> global_labels;
};

using CheckpointScopes =
    std::vector<std::vector<std::pair<std::string, std::pair<bool, std::string>>>>;

struct CheckpointV2 {
    VariableBank variables;
    CheckpointFlow flow;
    CheckpointScopes scopes;
    std::vector<SavedCommand> scene;
};

inline bool operator==(const CheckpointWait& a, const CheckpointWait& b) {
    return a.waiting == b.waiting && a.timed == b.timed &&
           a.accept_input == b.accept_input && a.announced == b.announced &&
           a.sound == b.sound && a.transition == b.transition &&
           a.sound_key == b.sound_key && a.video_key == b.video_key &&
           a.remaining_ms == b.remaining_ms && a.auto_elapsed == b.auto_elapsed;
}
inline bool operator==(const CheckpointFrame& a, const CheckpointFrame& b) {
    return a.file == b.file && a.pc == b.pc && a.halted == b.halted &&
           a.macro_scope == b.macro_scope && a.wait == b.wait;
}
inline bool operator==(const CheckpointFlow& a, const CheckpointFlow& b) {
    return a.current_file == b.current_file && a.pc == b.pc &&
           a.loaded == b.loaded && a.halted == b.halted &&
           a.pc_pending == b.pc_pending && a.frames == b.frames &&
           a.global_labels == b.global_labels;
}

bool EncodeCheckpointV2(const CheckpointV2&, std::vector<uint8_t>&, std::string&);
bool DecodeCheckpointV2(const std::vector<uint8_t>&, CheckpointV2&, std::string&);

} // namespace artc
