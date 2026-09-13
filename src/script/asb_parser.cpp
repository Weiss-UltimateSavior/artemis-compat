#include "script/asb_parser.h"
#include "script/lua_engine.h"

#include "log/logger.h"
#include "pack/pack_manager.h"

#include <cstring>

namespace artc {

namespace {
const char kMagic[4] = {'A', 'S', 'B', '\0'};
constexpr uint32_t kTypeTag = 0;
constexpr uint32_t kTypeLabel = 1;

struct Cursor {
    const std::vector<uint8_t> &d;
    size_t o;
    bool ok = true;

    explicit Cursor(const std::vector<uint8_t> &data, size_t off) : d(data), o(off) {}

    uint32_t U32() {
        if (!ok || o + 4 > d.size()) { ok = false; return 0; }
        const uint32_t v = static_cast<uint32_t>(d[o]) |
                           (static_cast<uint32_t>(d[o + 1]) << 8) |
                           (static_cast<uint32_t>(d[o + 2]) << 16) |
                           (static_cast<uint32_t>(d[o + 3]) << 24);
        o += 4;
        return v;
    }
    // Length-prefixed, NUL-terminated string.
    std::string Str() {
        const uint32_t len = U32();
        if (!ok || len > 4096 || o + len + 1 > d.size() || d[o + len] != 0) {
            ok = false;
            return {};
        }
        std::string s(reinterpret_cast<const char *>(d.data()) + o, len);
        o += len + 1;
        return s;
    }
};
} // namespace

// Format (behavior-level spec, verified against a full real script):
//   "ASB\0" + u8 pad + u32 line_count
//   line_count × line, each: u32 type (0 = tag, 1 = label) + body
//     label body: u32 len, name, NUL
//     tag body:   u32 len, name, NUL, u32 lineno, u32 nattrs,
//                 nattrs × (u32 klen, key, NUL, u32 vlen, val, NUL)
bool ParseAsb(const std::vector<uint8_t> &data, AsbScript *out) {
    if (data.size() < 13 || std::memcmp(data.data(), kMagic, 4) != 0) return false;
    out->lines.clear();
    out->labels.clear();
    Cursor c{data, 5};
    const uint32_t count = c.U32();
    for (uint32_t i = 0; i < count && c.ok; ++i) {
        const uint32_t type = c.U32();
        AsbLine line;
        line.is_label = (type == kTypeLabel);
        line.command = c.Str();
        if (!c.ok) break;
        if (line.is_label) {
            out->labels.emplace_back(line.command, out->lines.size());
        } else {
            line.lineno = static_cast<int>(c.U32());
            const uint32_t nattrs = c.U32();
            for (uint32_t a = 0; a < nattrs && c.ok; ++a) {
                std::string k = c.Str();
                std::string v = c.Str();
                line.attrs.emplace_back(std::move(k), std::move(v));
            }
        }
        if (!c.ok) {
            Log(kLogWarn, "asb: truncated at line " + std::to_string(i));
            break;
        }
        out->lines.push_back(std::move(line));
    }
    return !out->lines.empty();
}

bool AsbRunner::Load(const std::vector<uint8_t> &image, const std::string &label) {
    ++flow_revision_;
    const bool binary = image.size() > 4 && image[0] == 'A' && image[1] == 'S' &&
                        image[2] == 'B' && image[3] == '\0';
    const bool ok = binary ? ParseAsb(image, &script_)
                           : ParseIetScript(
                                 std::string(image.begin(), image.end()), &script_);
    if (!ok) {
        Log(kLogError, "asb: image parse failed");
        return false;
    }
    loaded_ = true;
    halted_ = false;
    pc_ = 0;
    if (!label.empty() && !FindLabel(label, &pc_)) {
        Log(kLogWarn, "asb: label not found: " + label);
        pc_ = 0;
    }
    return true;
}

bool AsbRunner::Jump(const std::string &file, const std::string &label) {
    if (!packs_) {
        Log(kLogError, "asb: no pack source for jump");
        return false;
    }
    if (std::getenv("ARTC_JUMP_TRACE"))
        Log(kLogInfo, "asb-jump: " + file + ":" + label);
    // The framework re-jumps to the same script every frame (click-wait poll);
    // reuse the parsed script when the file is unchanged.
    if (loaded_ && file == current_file_) {
        JumpTo(label);
        return true;
    }
    std::vector<uint8_t> image;
    if (!packs_->Read(file, image)) {
        Log(kLogError, "asb: script not found in packs: " + file);
        return false;
    }
    Log(kLogInfo, "asb: load " + file + " label=" + label);
    if (!Load(image, label)) return false;
    current_file_ = file;
    return true;
}

bool AsbRunner::Call(const std::string &file, const std::string &label) {
    // Only record a resume point when the caller's cursor is valid; a
    // Lua-originated estag call whose runner sits at a stale halt must not
    // push a return into a dead region.
    const bool event_start = event_entry_ && event_revision_ == flow_revision_;
    if (!event_start && loaded_ && pc_ + 1 < script_.lines.size())
        callstack_.push_back({current_file_, pc_ + 1});
    return Jump(file, label);
}

bool AsbRunner::Return() {
    if (callstack_.empty()) return false;
    const auto top = callstack_.back();
    callstack_.pop_back();
    if (top.file != current_file_) {
        // KrKr2-Next: cross-file return — reload the caller's script and
        // resume at the saved line. script.asb *movie_play does
        // `[call file="system/first.iet" label="movie_emergendcy"]` and
        // relies on the [return] landing back in script.asb; halting here
        // stranded the story after every (skipped) movie.
        std::vector<uint8_t> image;
        if (!packs_ || !packs_->Read(top.file, image)) {
            Log(kLogError, "asb: cannot reload caller script: " + top.file);
            return false;
        }
        if (!Load(image, "")) return false;
        current_file_ = top.file;
        Log(kLogInfo, "asb: return to " + top.file + " line " + std::to_string(top.pc));
    }
    if (top.pc >= script_.lines.size() && !top.event) return false;
    pc_ = top.pc;
    ++flow_revision_;
    halted_ = top.halted;
    if (top.lua) top.lua->RestoreWait(top.wait);
    return true;
}

uint64_t AsbRunner::BeginEvent(LuaEngine& lua) {
    if (!loaded_ || event_entry_) return 0;
    Frame frame{current_file_, pc_, halted_, ++next_event_, &lua, lua.SuspendWait()};
    callstack_.push_back(std::move(frame));
    event_entry_ = next_event_;
    event_revision_ = flow_revision_;
    return event_entry_;
}

void AsbRunner::DiscardFlow() {
    callstack_.clear();
    event_entry_=0;
    ++flow_revision_;
    loaded_=false;
    halted_=true;
}

void AsbRunner::EndEvent(uint64_t token) {
    if (!token || token != event_entry_) return;
    if (flow_revision_ == event_revision_ && !callstack_.empty() &&
        callstack_.back().event == token) {
        Return();
    }
    event_entry_ = 0;
}

std::vector<std::string> AsbRunner::StackFiles() const {
    std::vector<std::string> files;
    for (const auto& frame : callstack_) files.push_back(frame.file);
    if (loaded_) files.push_back(current_file_);
    return files;
}

void AsbRunner::ShiftWaitDeadlines(LuaEngine& lua, std::chrono::steady_clock::duration pause) {
    for (auto& frame : callstack_)
        if (frame.lua == &lua && frame.wait.timed) frame.wait.deadline += pause;
}

bool AsbRunner::FindLabel(const std::string &label, size_t *pc) {
    for (const auto &kv : script_.labels) {
        if (kv.first == label) { *pc = kv.second; return true; }
    }
    return false;
}

void AsbRunner::JumpTo(const std::string &label) {
    ++flow_revision_;
    // A jump re-enters execution (estag chains call the same script again
    // after an earlier [return] halted it — see AsbRunner::Jump's cache path).
    halted_ = false;
    if (std::getenv("ARTC_JUMP_TRACE"))
        Log(kLogInfo, "asb-jumpto: " + label + " pc=" + std::to_string(pc_));
    size_t pc = 0;
    if (FindLabel(label, &pc)) pc_ = pc;
    else Log(kLogWarn, "asb: jump target not found: " + label);
}

bool AsbRunner::ExecuteLine(LuaEngine& lua) {
    if (!loaded_ || halted_ || pc_ >= script_.lines.size()) { halted_ = true; return false; }
    const uint64_t before = flow_revision_;
    const AsbLine line = Current(); // callbacks can replace script_ in this call
    auto attr = [&](const char* name) {
        for (const auto& kv : line.attrs) if (kv.first == name) return kv.second;
        return std::string();
    };
    if (line.is_label) {
        Advance();
        return true;
    }
    if (line.command == "\x02LUA") lua.DoString(attr("code"), "asb:lua");
    else if (line.command == "calllua") lua.CallGlobal(attr("function"));
    else if (line.command == "jump" || line.command == "call") {
        const std::string file = attr("file").empty() ? current_file_ : attr("file");
        const bool ok = line.command == "call" ? Call(file, attr("label")) : Jump(file, attr("label"));
        if (!ok) Halt();
        return ok;
    } else if (line.command == "stop" && line.attrs.empty()) {
        Halt();
        lua.NotifyScriptStop();
        return true;
    } else if (line.command == "return") {
        if (!Return()) Halt();
        return true;
    } else if (line.command != "stop") {
        lua.DispatchTag(line.command, line.attrs);
    }
    if (before == flow_revision_ && !halted_) Advance();
    return true;
}

// ---- plain .iet text → AsbScript ----

namespace {

// Splits `inner` ("tag key="v" k2=v2") into command + attrs. Quoted values
// keep spaces; bare values run to the next space.
void ParseIetBracket(const std::string &inner, AsbLine *out) {
    size_t i = 0;
    while (i < inner.size() && (inner[i] == ' ' || inner[i] == '\t')) ++i;
    size_t start = i;
    while (i < inner.size() && inner[i] != ' ' && inner[i] != '\t') ++i;
    out->is_label = false;
    out->command = inner.substr(start, i - start);
    out->lineno = 0;
    out->attrs.clear();
    while (i < inner.size()) {
        while (i < inner.size() && (inner[i] == ' ' || inner[i] == '\t')) ++i;
        if (i >= inner.size()) break;
        start = i;
        while (i < inner.size() && inner[i] != '=' && inner[i] != ' ') ++i;
        if (i >= inner.size() || inner[i] == ' ') { // bare token — skip
            i = start;
            while (i < inner.size() && inner[i] != ' ') ++i;
            continue;
        }
        const std::string key = inner.substr(start, i - start);
        ++i; // '='
        std::string val;
        if (i < inner.size() && inner[i] == '"') {
            ++i;
            while (i < inner.size() && inner[i] != '"') val += inner[i++];
            if (i < inner.size()) ++i;
        } else {
            while (i < inner.size() && inner[i] != ' ') val += inner[i++];
        }
        out->attrs.emplace_back(key, val);
    }
}

} // namespace

bool ParseIetScript(const std::string &text, AsbScript *out) {
    out->lines.clear();
    out->labels.clear();
    size_t pos = 0;
    bool in_lua = false;
    std::string lua_code;
    while (pos <= text.size()) {
        size_t eol = text.find('\n', pos);
        std::string line = text.substr(pos, eol == std::string::npos
                                                ? std::string::npos : eol - pos);
        if (eol == std::string::npos) pos = text.size() + 1;
        else pos = eol + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();

        if (in_lua) {
            if (line == "[/lua]") {
                AsbLine l;
                l.command = "\x02LUA";
                l.attrs.emplace_back("code", lua_code);
                out->lines.push_back(std::move(l));
                in_lua = false;
            } else {
                lua_code += line;
                lua_code += '\n';
            }
            continue;
        }
        // comment / blank
        const std::string trimmed = line.substr(0, 2);
        if (line.empty() || trimmed == "//") continue;
        if (line.size() > 1 && line[0] == '*') {
            AsbLine l;
            l.is_label = true;
            l.command = line.substr(1);
            out->labels.emplace_back(l.command, out->lines.size());
            out->lines.push_back(std::move(l));
            continue;
        }
        if (line == "[lua]") { in_lua = true; lua_code.clear(); continue; }
        if (line.size() >= 2 && line.front() == '[' && line.back() == ']') {
            AsbLine l;
            ParseIetBracket(line.substr(1, line.size() - 2), &l);
            out->lines.push_back(std::move(l));
            continue;
        }
        // scenario text line (message layer, M3 next batch)
        AsbLine l;
        l.command = "\x01TEXT";
        l.attrs.emplace_back("text", line);
        out->lines.push_back(std::move(l));
    }
    return !out->lines.empty();
}

} // namespace artc
