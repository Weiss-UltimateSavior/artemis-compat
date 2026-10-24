#include "save/checkpoint.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <zlib.h>

namespace artc {
namespace {

constexpr size_t kMaxBytes = 64 * 1024 * 1024;
constexpr size_t kMaxString = 16 * 1024 * 1024;
constexpr uint32_t kMaxItems = 65536;

bool Fail(std::string& error, const std::string& message) {
    error = message;
    return false;
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void Put64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(value >> (i * 8)));
}
void PutBool(std::vector<uint8_t>& out, bool value) { out.push_back(value ? 1 : 0); }
void PutDouble(std::vector<uint8_t>& out, double value) {
    uint64_t bits = 0;
    static_assert(sizeof(bits) == sizeof(value), "double must be 64-bit");
    std::memcpy(&bits, &value, sizeof(bits));
    Put64(out, bits);
}
bool PutString(std::vector<uint8_t>& out, const std::string& value, std::string& error) {
    if (value.size() > kMaxString) return Fail(error, "checkpoint string is too large");
    Put32(out, static_cast<uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    return true;
}
void PutSection(std::vector<uint8_t>& out, const std::vector<uint8_t>& section) {
    Put32(out, static_cast<uint32_t>(section.size()));
    out.insert(out.end(), section.begin(), section.end());
}

bool PutWait(std::vector<uint8_t>& out, const CheckpointWait& wait, std::string& error) {
    if (wait.remaining_ms == std::numeric_limits<uint64_t>::max() ||
        !std::isfinite(wait.auto_elapsed)) {
        return Fail(error, "invalid checkpoint wait");
    }
    for (bool flag : {wait.waiting, wait.timed, wait.accept_input,
                      wait.announced, wait.sound, wait.transition}) PutBool(out, flag);
    if (!PutString(out, wait.sound_key, error) || !PutString(out, wait.video_key, error)) return false;
    Put64(out, wait.remaining_ms);
    PutDouble(out, wait.auto_elapsed);
    return true;
}

bool PutFlow(std::vector<uint8_t>& out, const CheckpointFlow& flow, std::string& error) {
    if (flow.frames.size() > kMaxItems || flow.global_labels.size() > kMaxItems ||
        flow.pc == std::numeric_limits<size_t>::max()) {
        return Fail(error, "invalid runner flow");
    }
    if (!PutString(out, flow.current_file, error)) return false;
    Put64(out, flow.pc);
    for (bool flag : {flow.loaded, flow.halted, flow.pc_pending}) PutBool(out, flag);
    Put32(out, static_cast<uint32_t>(flow.global_labels.size()));
    for (const auto& entry : flow.global_labels) {
        if (!PutString(out, entry.first, error) || !PutString(out, entry.second, error)) return false;
    }
    Put32(out, static_cast<uint32_t>(flow.frames.size()));
    for (const auto& frame : flow.frames) {
        if (frame.pc == std::numeric_limits<size_t>::max()) return Fail(error, "invalid frame PC");
        if (!PutString(out, frame.file, error)) return false;
        Put64(out, frame.pc);
        PutBool(out, frame.halted);
        PutBool(out, frame.macro_scope);
        if (!PutWait(out, frame.wait, error)) return false;
    }
    return true;
}

bool PutVariables(std::vector<uint8_t>& out, const VariableBank& variables, std::string& error) {
    if (variables.size() > kMaxItems) return Fail(error, "too many checkpoint variables");
    Put32(out, static_cast<uint32_t>(variables.size()));
    for (const auto& entry : variables) {
        if (!PutString(out, entry.first, error) || !PutString(out, entry.second, error)) return false;
    }
    return true;
}

bool PutScopes(std::vector<uint8_t>& out, const CheckpointScopes& scopes, std::string& error) {
    if (scopes.size() > kMaxItems) return Fail(error, "too many macro scopes");
    Put32(out, static_cast<uint32_t>(scopes.size()));
    for (const auto& scope : scopes) {
        if (scope.size() > kMaxItems) return Fail(error, "too many macro scope entries");
        Put32(out, static_cast<uint32_t>(scope.size()));
        for (const auto& entry : scope) {
            if (!PutString(out, entry.first, error)) return false;
            PutBool(out, entry.second.first);
            if (!PutString(out, entry.second.second, error)) return false;
        }
    }
    return true;
}

bool FiniteNumber(const std::string& value) {
    if (value.empty()) return false;
    char *end = nullptr;
    const double parsed = std::strtod(value.c_str(), &end);
    return end == value.c_str() + value.size() && std::isfinite(parsed);
}

bool PutScene(std::vector<uint8_t>& out, const std::vector<SavedCommand>& scene,
              std::string& error) {
    if (scene.size() > kMaxItems) return Fail(error, "too many scene commands");
    Put32(out, static_cast<uint32_t>(scene.size()));
    for (const auto& command : scene) {
        if (command.attrs.size() > kMaxItems) return Fail(error, "too many scene attributes");
        if (!PutString(out, command.name, error)) return false;
        Put32(out, static_cast<uint32_t>(command.attrs.size()));
        for (const auto& entry : command.attrs) {
            if ((entry.first == "left" || entry.first == "top" ||
                 entry.first == "width" || entry.first == "height" ||
                 entry.first == "alpha") && !FiniteNumber(entry.second)) {
                return Fail(error, "non-finite scene geometry");
            }
            if (!PutString(out, entry.first, error) || !PutString(out, entry.second, error)) return false;
        }
    }
    return true;
}

struct Reader {
    const std::vector<uint8_t>& data;
    size_t pos = 0;
    size_t end = 0;
    std::string& error;

    bool Bytes(size_t count) {
        if (count > end - pos) return Fail(error, "truncated checkpoint");
        return true;
    }
    bool U32(uint32_t& value) {
        if (!Bytes(4)) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data[pos++]) << (i * 8);
        return true;
    }
    bool U64(uint64_t& value) {
        if (!Bytes(8)) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data[pos++]) << (i * 8);
        return true;
    }
    bool Index(size_t& value) {
        uint64_t encoded = 0;
        if (!U64(encoded)) return false;
        if (encoded > std::numeric_limits<size_t>::max()) return Fail(error, "checkpoint index is out of range");
        value = static_cast<size_t>(encoded);
        return true;
    }
    bool Bool(bool& value) {
        if (!Bytes(1)) return false;
        const uint8_t raw = data[pos++];
        if (raw > 1) return Fail(error, "invalid checkpoint boolean");
        value = raw != 0;
        return true;
    }
    bool Double(double& value) {
        uint64_t bits = 0;
        if (!U64(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return std::isfinite(value) || Fail(error, "non-finite checkpoint number");
    }
    bool Count(uint32_t& value) {
        if (!U32(value)) return false;
        return value <= kMaxItems || Fail(error, "too many checkpoint entries");
    }
    bool String(std::string& value) {
        uint32_t size = 0;
        if (!U32(size)) return false;
        if (size > kMaxString) return Fail(error, "checkpoint string is too large");
        if (!Bytes(size)) return false;
        value.assign(reinterpret_cast<const char*>(data.data() + pos), size);
        pos += size;
        return true;
    }
    bool Section(Reader& child) {
        uint32_t size = 0;
        if (!U32(size) || !Bytes(size)) return false;
        child.pos = pos;
        child.end = pos + size;
        pos += size;
        return true;
    }
    bool Done() const { return pos == end; }
};

bool GetWait(Reader& in, CheckpointWait& wait) {
    for (bool *flag : {&wait.waiting, &wait.timed, &wait.accept_input,
                       &wait.announced, &wait.sound, &wait.transition}) {
        if (!in.Bool(*flag)) return false;
    }
    if (!in.String(wait.sound_key) || !in.String(wait.video_key) ||
        !in.U64(wait.remaining_ms) || !in.Double(wait.auto_elapsed)) return false;
    return wait.remaining_ms != std::numeric_limits<uint64_t>::max() ||
           Fail(in.error, "invalid checkpoint wait");
}

bool GetFlow(Reader& in, CheckpointFlow& flow) {
    if (!in.String(flow.current_file) || !in.Index(flow.pc)) return false;
    if (flow.pc == std::numeric_limits<size_t>::max()) return Fail(in.error, "invalid current PC");
    for (bool *flag : {&flow.loaded, &flow.halted, &flow.pc_pending}) {
        if (!in.Bool(*flag)) return false;
    }
    uint32_t count = 0;
    if (!in.Count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::string label, file;
        if (!in.String(label) || !in.String(file)) return false;
        if (!flow.global_labels.emplace(std::move(label), std::move(file)).second) {
            return Fail(in.error, "duplicate global label");
        }
    }
    if (!in.Count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        CheckpointFrame frame;
        if (!in.String(frame.file) || !in.Index(frame.pc)) return false;
        if (frame.pc == std::numeric_limits<size_t>::max()) return Fail(in.error, "invalid frame PC");
        if (!in.Bool(frame.halted) || !in.Bool(frame.macro_scope) || !GetWait(in, frame.wait)) return false;
        flow.frames.push_back(std::move(frame));
    }
    return in.Done();
}

bool GetVariables(Reader& in, VariableBank& variables) {
    uint32_t count = 0;
    if (!in.Count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        std::string key, value;
        if (!in.String(key) || !in.String(value)) return false;
        if (!variables.emplace(std::move(key), std::move(value)).second) {
            return Fail(in.error, "duplicate checkpoint variable");
        }
    }
    return in.Done();
}

bool GetScopes(Reader& in, CheckpointScopes& scopes) {
    uint32_t count = 0;
    if (!in.Count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t entries = 0;
        if (!in.Count(entries)) return false;
        CheckpointScopes::value_type scope;
        for (uint32_t j = 0; j < entries; ++j) {
            std::string name, previous;
            bool existed = false;
            if (!in.String(name) || !in.Bool(existed) || !in.String(previous)) return false;
            scope.emplace_back(std::move(name), std::make_pair(existed, std::move(previous)));
        }
        scopes.push_back(std::move(scope));
    }
    return in.Done();
}

bool GetScene(Reader& in, std::vector<SavedCommand>& scene) {
    uint32_t count = 0;
    if (!in.Count(count)) return false;
    for (uint32_t i = 0; i < count; ++i) {
        SavedCommand command;
        uint32_t attrs = 0;
        if (!in.String(command.name) || !in.Count(attrs)) return false;
        for (uint32_t j = 0; j < attrs; ++j) {
            std::string key, value;
            if (!in.String(key) || !in.String(value)) return false;
            if ((key == "left" || key == "top" || key == "width" ||
                 key == "height" || key == "alpha") && !FiniteNumber(value)) {
                return Fail(in.error, "non-finite scene geometry");
            }
            if (!command.attrs.emplace(std::move(key), std::move(value)).second) {
                return Fail(in.error, "duplicate scene attribute");
            }
        }
        scene.push_back(std::move(command));
    }
    return in.Done();
}

} // namespace

bool EncodeCheckpointV2(const CheckpointV2& input, std::vector<uint8_t>& output,
                        std::string& error) {
    error.clear();
    output.clear();
    std::vector<uint8_t> variables, flow, scopes, scene;
    if (!PutVariables(variables, input.variables, error) ||
        !PutFlow(flow, input.flow, error) ||
        !PutScopes(scopes, input.scopes, error) ||
        !PutScene(scene, input.scene, error)) return false;
    output = {'A', 'R', 'C', 'V'};
    Put32(output, 2);
    PutSection(output, variables);
    PutSection(output, flow);
    PutSection(output, scopes);
    PutSection(output, scene);
    if (output.size() + 4 > kMaxBytes) return Fail(error, "checkpoint is too large");
    Put32(output, crc32(0, output.data(), output.size()));
    return true;
}

bool DecodeCheckpointV2(const std::vector<uint8_t>& bytes, CheckpointV2& output,
                        std::string& error) {
    error.clear();
    if (bytes.size() < 12 || bytes.size() > kMaxBytes) return Fail(error, "invalid checkpoint size");
    if (std::memcmp(bytes.data(), "ARCV", 4) != 0) return Fail(error, "invalid checkpoint magic");
    Reader in{bytes, 4, bytes.size(), error};
    uint32_t version = 0;
    if (!in.U32(version) || version != 2) return Fail(error, "unsupported checkpoint version");
    const uint32_t expected = crc32(0, bytes.data(), bytes.size() - 4);
    uint32_t actual = 0;
    for (int i = 0; i < 4; ++i) actual |= static_cast<uint32_t>(bytes[bytes.size() - 4 + i]) << (i * 8);
    if (actual != expected) return Fail(error, "checkpoint CRC mismatch");
    in.end = bytes.size() - 4;

    CheckpointV2 next;
    Reader variables{bytes, 0, 0, error}, flow{bytes, 0, 0, error};
    Reader scopes{bytes, 0, 0, error}, scene{bytes, 0, 0, error};
    if (!in.Section(variables) || !in.Section(flow) || !in.Section(scopes) || !in.Section(scene)) return false;
    if (!GetVariables(variables, next.variables) || !GetFlow(flow, next.flow) ||
        !GetScopes(scopes, next.scopes) || !GetScene(scene, next.scene)) return false;
    if (!in.Done()) return Fail(error, "trailing checkpoint data");
    output = std::move(next);
    return true;
}

} // namespace artc
