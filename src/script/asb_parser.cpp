#include "script/asb_parser.h"
#include "script/iet_parser.h"
#include "script/lua_engine.h"
#include "script/preprocess.h"
#include "util/encoding.h"

#include "log/logger.h"
#include "pack/pack_manager.h"

#include <cstring>
#include <cctype>
#include <cstdlib>

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

// ASB record layout accepted by this parser:
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
                           : [&] {
                                 std::string decoded;
                                 DecodeToUtf8(std::string(image.begin(), image.end()),
                                              TextCharset(), decoded);
                                 return ParseIetScript(PreprocessScript(decoded), &script_);
                             }();
    if (!ok) {
        Log(kLogError, "asb: image parse failed");
        return false;
    }
    loaded_ = true;
    halted_ = false;
    pc_ = 0;
    pc_pending_ = false;
    lua_chunks_loaded_ = !binary && lua_ != nullptr;
    if (lua_chunks_loaded_) RunLoadTimeLuaChunks();
    if (!label.empty() && !FindLabel(label, &pc_)) {
        Log(kLogWarn, "asb: label not found: " + label);
        pc_ = 0;
    }
    return true;
}

void AsbRunner::RunLoadTimeLuaChunks() {
    for (const auto &line : script_.lines) {
        if (!line.is_label && line.command == "\x02LUA") {
            for (const auto &kv : line.attrs) {
                if (kv.first == "code") {
                    lua_->DoString(kv.second, "iet:load-lua");
                    break;
                }
            }
        }
    }
}

bool AsbRunner::Jump(const std::string &file, const std::string &label) {
    if (!packs_) {
        Log(kLogError, "asb: no pack source for jump");
        return false;
    }
    // A jump whose file is omitted resolves a bare label: prefer the current
    // script (official same-file semantics), then the cross-file index built
    // from the boot macro scripts. A still-unresolved target with no script
    // loaded at all is a malformed placeholder: no-op instead of permanently
    // halting the runner on a black boot screen.
    if (file.empty()) {
        if (loaded_) {
            size_t pc = 0;
            if (FindLabel(label, &pc)) {
                JumpTo(label);
                return true;
            }
            std::string global_file;
            if (ResolveGlobalLabel(label, &global_file)) {
                Log(kLogInfo, "asb: bare label " + label + " -> " + global_file);
                return Jump(global_file, label);
            }
        }
        Log(kLogWarn, "asb: ignoring unresolved bare jump (label=" + label + ")");
        return true;
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
    IndexLoadedLabels();
    return true;
}

bool AsbRunner::LoadBootAnchor(const std::string &file) {
    if (!packs_) return false;
    std::vector<uint8_t> image;
    if (!packs_->Read(file, image)) {
        Log(kLogError, "asb: boot anchor not found in packs: " + file);
        return false;
    }
    if (!Load(image, "")) return false;
    current_file_ = file;
    for (const auto &lp : script_.labels)
        global_labels_.emplace(lp.first, file);  // insert-only, see header
    halted_ = true;
    return true;
}

void AsbRunner::IndexLoadedLabels() {
    if (current_file_.empty()) return;
    for (const auto &lp : script_.labels)
        global_labels_[lp.first] = current_file_;
}

bool AsbRunner::ResolveGlobalLabel(const std::string &label, std::string *file) {
    const auto it = global_labels_.find(label);
    if (it == global_labels_.end()) return false;
    if (file) *file = it->second;
    return true;
}

namespace {
bool WaitActive(const LuaEngine::WaitState &w) {
    return w.waiting || w.timed || w.sound || w.transition ||
           !w.video_key.empty();
}
struct FlagGuard {
    bool &flag;
    explicit FlagGuard(bool &f) : flag(f) { flag = true; }
    ~FlagGuard() { flag = false; }
};
} // namespace

bool AsbRunner::Call(const std::string &file, const std::string &label) {
    // A context-less empty call with no script loaded is a malformed
    // placeholder: do not push a bogus return frame. When a script is loaded,
    // fall through so bare-label resolution (see Jump) applies.
    if (file.empty() && !loaded_) {
        Log(kLogWarn, "asb: ignoring unresolved bare call (label=" + label + ")");
        return true;
    }
    // Resume point: inside ExecuteLine the cursor still sits on the calling
    // line (it advances after the transfer), so resume after it; a parked
    // runner (Lua-originated call) already points at the next line, and a
    // pc_pending_ cursor (just after [return]) must resume at pc_ itself.
    // The resume < size guard keeps a Lua-originated estag call whose runner
    // sits at a stale halt from pushing a return into a dead region.
    const size_t resume = (pc_pending_ || !executing_) ? pc_ : pc_ + 1;
    const bool push = loaded_ && resume < script_.lines.size();
    if (push) {
        Frame frame{current_file_, resume, halted_, lua_, {}};
        // The caller's wait travels with the frame and is handed back when
        // the callee returns: an event handler (menu open) suspends the
        // click wait before calling into UI scripts, and the framework's
        // getScriptStack()-driven return count relies on one frame per call.
        if (lua_) frame.wait = lua_->SuspendWait();
        if (event_wait_valid_) {
            if (!WaitActive(frame.wait) && WaitActive(event_wait_))
                frame.wait = event_wait_;
            event_wait_valid_ = false;
        }
        callstack_.push_back(std::move(frame));
    }
    return Jump(file, label);
}

bool AsbRunner::Return() {
    if (callstack_.empty()) return false;
    const auto top = callstack_.back();
    callstack_.pop_back();
    // A macro frame carries the attribute scope seeded at the call site.
    if (top.macro_scope && top.lua) top.lua->PopVarScope();
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
    if (top.pc >= script_.lines.size()) return false;
    pc_ = top.pc;
    pc_pending_ = true;
    ++flow_revision_;
    halted_ = top.halted;
    if (top.lua) top.lua->RestoreWait(top.wait);
    return true;
}

uint64_t AsbRunner::BeginEvent(LuaEngine& lua) {
    if (!loaded_ || event_entry_) return 0;
    event_wait_ = lua.SuspendWait();
    event_wait_valid_ = true;
    event_lua_ = &lua;
    event_file_ = current_file_;
    event_pc_ = pc_;
    event_halted_ = halted_;
    event_entry_ = ++next_event_;
    event_revision_ = flow_revision_;
    return event_entry_;
}

void AsbRunner::DiscardFlow() {
    callstack_.clear();
    event_entry_=0;
    event_wait_valid_=false;
    event_lua_=nullptr;
    ++flow_revision_;
    loaded_=false;
    halted_=true;
}

void AsbRunner::EndEvent(uint64_t token) {
    if (!token || token != event_entry_) return;
    if (flow_revision_ == event_revision_) {
        // Handler made no control transfer: hand the suspended wait back.
        if (event_wait_valid_ && event_lua_) event_lua_->RestoreWait(event_wait_);
    } else if (event_wait_valid_ && event_lua_ && !event_halted_) {
        // The handler transferred control without a call adopting the wait
        // (plain jump to a menu script): the interrupted position becomes a
        // single return frame so the target's [return] lands back here.
        // Exactly one frame is pushed, keeping the framework's
        // getScriptStack() pairing (story below menu) intact.
        //
        // A position already halted at [stop] (event_halted_) has no
        // continuation: the yesno dialog framework parks dialog_open at
        // [stop], then jumps from the click handler to dialog_close and
        // expects dialog_close's [return] to land at the real caller below
        // (adv_title's resume line). Preserving the dead frame would strand
        // that [return] on the [stop] and freeze the runner.
        callstack_.push_back({event_file_, event_pc_, event_halted_,
                              event_lua_, event_wait_});
    }
    // A call-transferred handler already adopted the wait into its frame.
    event_wait_valid_ = false;
    event_lua_ = nullptr;
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
    if (event_wait_valid_ && event_lua_ == &lua && event_wait_.timed)
        event_wait_.deadline += pause;
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
    pc_pending_ = false;
    if (std::getenv("ARTC_JUMP_TRACE"))
        Log(kLogInfo, "asb-jumpto: " + label + " pc=" + std::to_string(pc_));
    size_t pc = 0;
    if (FindLabel(label, &pc)) pc_ = pc;
    else Log(kLogWarn, "asb: jump target not found: " + label);
}

void AsbRunner::GotoIndex(size_t index) {
    if (index > script_.lines.size()) {
        Log(kLogWarn, "asb: compiled branch target out of range: " +
                          std::to_string(index));
        Halt();
        return;
    }
    ++flow_revision_;
    halted_ = false;
    pc_pending_ = false;
    pc_ = index;
}

namespace {
// Compiled ASB branch/loop metadata is keyed with a leading vertical tab.
constexpr const char *kBranchPrefix = "\x0b";

std::string Attr(const AsbLine &line, const char *name) {
    for (const auto &kv : line.attrs)
        if (kv.first == name) return kv.second;
    return std::string();
}

// `estimate` without a leading '$' is a plain numeric parameter: it reads the
// leading integer, or 0 when there is none (verified against the original
// engine). With a '$' it is an Artemis expression.
bool EstimateTrue(LuaEngine &lua, const std::string &estimate) {
    if (estimate.empty()) return true;
    if (estimate.front() != '$') {
        size_t i = 0;
        while (i < estimate.size() &&
               std::isspace(static_cast<unsigned char>(estimate[i]))) ++i;
        const size_t start = i;
        if (i < estimate.size() && (estimate[i] == '+' || estimate[i] == '-')) ++i;
        const size_t digits = i;
        while (i < estimate.size() && std::isdigit(static_cast<unsigned char>(estimate[i]))) ++i;
        if (i == digits) return false; // no leading number -> 0
        return std::strtoll(estimate.substr(start, i - start).c_str(), nullptr, 10) != 0;
    }
    const std::string result = lua.ResolveValue(estimate);
    if (result.empty()) return false;
    char *end = nullptr;
    const long long n = std::strtoll(result.c_str(), &end, 0);
    if (end == result.c_str() + result.size()) return n != 0;
    return true; // non-numeric, non-empty string is truthy
}
} // namespace

bool AsbRunner::ExecuteLine(LuaEngine& lua) {
    if (!loaded_ || halted_ || pc_ >= script_.lines.size()) { halted_ = true; return false; }
    FlagGuard exec_guard(executing_);
    pc_pending_ = false;
    const uint64_t before = flow_revision_;
    const AsbLine line = Current(); // callbacks can replace script_ in this call
    auto attr = [&](const char* name) { return Attr(line, name); };
    if (line.is_label) {
        Advance();
        return true;
    }
    // Compiled control flow. Text .iet scripts have no \x0bindex metadata, so
    // this only engages for compiled records and leaves text behavior intact.
    const std::string index_attr =
        attr((std::string(kBranchPrefix) + "index").c_str());
    const std::string branch_index = index_attr.empty() ? attr("index") : index_attr;
    if (line.command == "\x02LUA") {
        if (!lua_chunks_loaded_) lua.DoString(attr("code"), "asb:lua");
    }
    else if (line.command == "calllua") {
        // Framework convention fn(e, attrs) — the same shape TagCallLua uses
        // for e:tag{"calllua"}: the attribute table is param 2. load_start
        // reads param.file, load_exec reads param["0"] for the valueless
        // `suspend` marker. Attribute values may reference variables
        // (file="$t.file"), so resolve them like EstimateTrue does.
        std::vector<std::pair<std::string, std::string>> params;
        params.reserve(line.attrs.size());
        for (const auto &kv : line.attrs)
            params.emplace_back(kv.first, lua.ResolveValue(kv.second));
        lua.CallEvent(attr("function"), params, false);
    }
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
    } else if (line.command == "if" || line.command == "elseif" ||
               line.command == "loop") {
        if (!branch_index.empty()) {
            if (!EstimateTrue(lua, attr("estimate"))) {
                GotoIndex(static_cast<size_t>(std::strtoull(branch_index.c_str(), nullptr, 10)));
                return true;
            }
            // condition true: fall through into the branch body
        } else if (line.command != "stop") {
            lua.DispatchTag(line.command, line.attrs);
        }
    } else if (line.command == std::string(kBranchPrefix) + "goto" ||
               line.command == "goto") {
        if (branch_index.empty()) {
            lua.DispatchTag(line.command, line.attrs);
        } else {
            GotoIndex(static_cast<size_t>(std::strtoull(branch_index.c_str(), nullptr, 10)));
            return true;
        }
    } else if (line.command == "else") {
        // fall through (reached only when no earlier branch was taken)
    } else if (line.command != "stop") {
        // Text .iet macro dispatch (KAG semantics — a macro overrides the
        // built-in tag of the same name): a tag matching a label of the
        // current script, or of any script loaded so far (macro.iet & co.
        // are loaded at boot), is a subroutine call. The tag's attributes
        // seed a variable scope for the macro body ([終端 time=1000] reads
        // $time; [yesno file=title] forwards $file to its calllua), popped
        // when the macro frame returns.
        size_t macro_pc = 0;
        std::string macro_file;
        if (FindLabel(line.command, &macro_pc) ||
            ResolveGlobalLabel(line.command, &macro_file)) {
            if (macro_file.empty()) macro_file = current_file_;
            lua.PushVarScope(line.attrs);
            const size_t depth = callstack_.size();
            if (!Call(macro_file, line.command)) {
                lua.PopVarScope();
                Halt();
                return false;
            }
            if (callstack_.size() > depth) callstack_.back().macro_scope = true;
            return true;
        }
        lua.DispatchTag(line.command, line.attrs);
    }
    if (before == flow_revision_ && !halted_) Advance();
    return true;
}

// ---- plain .iet text → AsbScript ----

namespace {

void ParseIetBracket(const std::string &inner, AsbLine *out) {
    out->is_label = false;
    out->lineno = 0;
    ParseIetInstruction(inner, out->command, out->attrs);
}

} // namespace

bool ParseIetScript(const std::string &text, AsbScript *out) {
    out->lines.clear();
    out->labels.clear();

    // Text .iet conditionals ([if]/[elseif]/[else]/[/if]) compile to the same
    // \x0bindex metadata the compiled-ASB path consumes: each conditional
    // header carries the line index to jump to when its estimate is false
    // (the next sibling header, or past the group), and each taken branch
    // ends with a synthetic \x0bgoto past the group. Without this the branch
    // bodies used to run unconditionally.
    struct IfCtx {
        bool has_pending = false;  // header awaiting its false target
        size_t pending = 0;
        std::vector<size_t> gotos; // synthetic gotos awaiting the group end
    };
    std::vector<IfCtx> ifs;
    auto set_index = [&](size_t line_no, size_t target) {
        out->lines[line_no].attrs.emplace_back(
            std::string(kBranchPrefix) + "index", std::to_string(target));
    };
    auto emit = [&](AsbLine l) {
        if (l.command == "if") {
            IfCtx ctx;
            ctx.has_pending = true;
            ctx.pending = out->lines.size();
            ifs.push_back(ctx);
            out->lines.push_back(std::move(l));
        } else if (l.command == "elseif" || l.command == "else") {
            if (!ifs.empty()) {
                IfCtx &ctx = ifs.back();
                // End the previous branch body: when its header's condition
                // held, skip the remaining branches.
                AsbLine g;
                g.command = std::string(kBranchPrefix) + "goto";
                ctx.gotos.push_back(out->lines.size());
                out->lines.push_back(std::move(g));
                // The previous header's false branch starts at this line.
                if (ctx.has_pending) set_index(ctx.pending, out->lines.size());
                if (l.command == "elseif") {
                    ctx.has_pending = true;
                    ctx.pending = out->lines.size();
                } else {
                    ctx.has_pending = false;
                }
            }
            out->lines.push_back(std::move(l));
        } else if (l.command == "/if") {
            if (!ifs.empty()) {
                IfCtx &ctx = ifs.back();
                const size_t end = out->lines.size();
                if (ctx.has_pending) set_index(ctx.pending, end);
                for (const size_t g : ctx.gotos) set_index(g, end);
                ifs.pop_back();
            }
            // [/if] itself emits no line.
        } else {
            out->lines.push_back(std::move(l));
        }
    };

    size_t pos = 0;
    bool in_lua = false;
    bool in_block_comment = false;
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
                emit(std::move(l));
                in_lua = false;
            } else {
                lua_code += line;
                lua_code += '\n';
            }
            continue;
        }
        // /* */ block comment (macro.iet comments out its deprecated macros
        // this way — including a second, dead *終端 definition).
        if (in_block_comment) {
            if (line.find("*/") != std::string::npos) in_block_comment = false;
            continue;
        }
        if (line.rfind("/*", 0) == 0) {
            if (line.find("*/") == std::string::npos) in_block_comment = true;
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
        if (line.size() >= 2 && line.front() == '[') {
            // Split the physical line into its bracket groups: text .iet
            // scripts put a whole one-line branch on a single line
            // ([if estimate=…][var …][/if]), and tolerate a trailing `//`
            // comment after the last group (system/system.iet adv_save ends
            // branches with `[ui_return]	// uiに戻る` — treating that as
            // scenario text silently drops the tag).
            size_t j = 0;
            std::vector<std::string> groups;
            bool balanced = true;
            while (j < line.size() && line[j] == '[') {
                size_t depth = 0, k = j;
                bool in_quote = false;
                for (; k < line.size(); ++k) {
                    const char c = line[k];
                    if (in_quote) { if (c == '"') in_quote = false; continue; }
                    if (c == '"') { in_quote = true; continue; }
                    if (c == '[') ++depth;
                    else if (c == ']') { --depth; if (depth == 0) { ++k; break; } }
                }
                if (depth != 0) { balanced = false; break; }
                groups.push_back(line.substr(j + 1, k - j - 2));
                j = k;
                while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) ++j;
            }
            std::string rest = line.substr(j);
            const size_t a = rest.find_first_not_of(" \t");
            rest = (a == std::string::npos) ? std::string() : rest.substr(a);
            if (balanced && !groups.empty() &&
                (rest.empty() || rest.rfind("//", 0) == 0)) {
                for (const auto &g : groups) {
                    AsbLine l;
                    ParseIetBracket(g, &l);
                    emit(std::move(l));
                }
                continue;
            }
        }
        // scenario text line (message layer, M3 next batch)
        AsbLine l;
        l.command = "\x01TEXT";
        l.attrs.emplace_back("text", line);
        emit(std::move(l));
    }
    // Unclosed conditional groups jump to the end of the script.
    while (!ifs.empty()) {
        IfCtx &ctx = ifs.back();
        const size_t end = out->lines.size();
        if (ctx.has_pending) set_index(ctx.pending, end);
        for (const size_t g : ctx.gotos) set_index(g, end);
        ifs.pop_back();
    }
    return !out->lines.empty();
}

} // namespace artc
