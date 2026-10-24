// checkpoint_regressions.cpp — ARCV v2 container and validation.
#include "save/checkpoint.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
} // namespace

int main() {
    artc::CheckpointV2 input;
    input.variables["lua.scr"] = std::string("a\0b", 3);
    input.flow.current_file = "scenario/main.iet";
    input.flow.pc = 7;
    input.flow.loaded = true;
    input.flow.frames.push_back({"system/ui.iet", 3, false, true,
                                 {true, true, false, true, false, false,
                                  "bgm", "", 125, 40}});
    input.flow.global_labels["game_start"] = "scenario/main.iet";
    input.scopes.push_back({{"time", {true, "1000"}}, {"name", {false, ""}}});
    input.scene.push_back({"lyc", {{"id", "bg"}, {"file", "image/bg"}}});
    input.scene.push_back({"print", {{"data", "restored"}}});

    artc::CheckpointV2 output;
    std::vector<uint8_t> bytes;
    std::string error;
    Check(artc::EncodeCheckpointV2(input, bytes, error), error);
    Check(artc::DecodeCheckpointV2(bytes, output, error), error);
    Check(output.variables == input.variables, "binary variable round trip");
    Check(output.flow == input.flow, "runner flow round trip");
    Check(output.scopes == input.scopes, "macro scope round trip");
    Check(output.scene.size() == 2 && output.scene[1].attrs.at("data") == "restored",
          "scene command round trip");

    auto rejected = [&](const std::vector<uint8_t>& candidate, const char *name) {
        artc::CheckpointV2 decoded;
        std::string reason;
        Check(!artc::DecodeCheckpointV2(candidate, decoded, reason),
              std::string("reject ") + name);
    };
    auto bad_magic = bytes;
    bad_magic[0] = 'X';
    rejected(bad_magic, "magic");
    auto bad_version = bytes;
    bad_version[4] = 1;
    rejected(bad_version, "version");
    auto bad_crc = bytes;
    bad_crc.back() ^= 1;
    rejected(bad_crc, "CRC");
    rejected(std::vector<uint8_t>(bytes.begin(), bytes.end() - 1), "truncation");
    auto trailing = bytes;
    trailing.push_back(0);
    rejected(trailing, "trailing bytes");

    artc::CheckpointV2 invalid = input;
    invalid.flow.pc = std::numeric_limits<size_t>::max();
    Check(!artc::EncodeCheckpointV2(invalid, bytes, error), "invalid current PC");
    invalid = input;
    invalid.flow.frames[0].wait.remaining_ms = std::numeric_limits<uint64_t>::max();
    Check(!artc::EncodeCheckpointV2(invalid, bytes, error), "invalid remaining wait");
    invalid = input;
    invalid.scene[0].attrs["left"] = "nan";
    Check(!artc::EncodeCheckpointV2(invalid, bytes, error), "non-finite scene geometry");
    invalid = input;
    invalid.scene[0].attrs["file"] = std::string(16 * 1024 * 1024 + 1, 'x');
    Check(!artc::EncodeCheckpointV2(invalid, bytes, error), "oversized string");

    if (g_failures == 0) std::cout << "checkpoint_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
