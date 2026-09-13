// asb_parser.h — compiled .asb script (behavior-level format spec, verified
// against the full system/script.asb of a real title):
//   header  : "ASB\0" + u32 line_count
//   label   : u32 1, u32 len, name, NUL, u32 0
//   tag     : u32 len, name, NUL, u32 lineno, u32 nattrs,
//             nattrs × (u32 klen, key, NUL, u32 vlen, val, NUL)
//   a u32 0 end-marker follows a tag line (absent when the next line is a
//   label — resolved with lookahead during parse).
// The engine executes the tag stack natively: calllua → Lua global, jump →
// reposition, stop → halt; every other tag dispatches through the e bridge.
#pragma once
#include "script/lua_engine.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace artc {

class PackManager;
class LuaEngine;

struct AsbLine {
    bool is_label = false;
    std::string command;                                     // label / tag name
    int lineno = 0;                                          // source line
    std::vector<std::pair<std::string, std::string>> attrs;  // key = value
};

struct AsbScript {
    std::vector<AsbLine> lines;
    std::vector<std::pair<std::string, size_t>> labels;      // name → line index
};

// Parses a full .asb image. Returns false on structural mismatch.
bool ParseAsb(const std::vector<uint8_t> &data, AsbScript *out);

// Parses plain .iet text (comments / *labels / [tag k="v"] / text) into the
// same line model, so one runner executes both script kinds. [lua]…[/lua]
// chunks become lines with command "\x02LUA" and a "code" attribute.
bool ParseIetScript(const std::string &text, AsbScript *out);

// Loaded-script cursor; the host (native_activity) executes each line.
class AsbRunner {
public:
    void SetPackSource(PackManager *packs) { packs_ = packs; }
    // Read `file` from the pack chain, parse, and seek `label`.
    bool Jump(const std::string &file, const std::string &label);
    // Jump with a return address, including cross-file calls.
    bool Call(const std::string &file, const std::string &label);
    // Pop a call frame and resume the caller. Returns false when the stack is
    // empty (a plain script end).
    bool Return();
    bool Loaded() const { return loaded_; }
    bool Halted() const { return halted_; }
    const std::vector<std::pair<std::string, size_t>> &Labels() const {
        return script_.labels;
    }
    const AsbLine &Current() const { return script_.lines[pc_]; }
    size_t CurrentIndex() const { return pc_; }
    void Advance() { if (++pc_ >= script_.lines.size()) halted_ = true; }
    // One native instruction. Lua may jump/call/return while the instruction
    // runs; do not retain references into the script or advance its new cursor.
    bool ExecuteLine(LuaEngine& lua);
    void JumpTo(const std::string &label);
    void Halt() { halted_ = true; }
    // A load replaces the old scenario and any suspended menu/event frames.
    void DiscardFlow();
    // External events may yield into native script commands. A handler with
    // no control transfer is completed synchronously by EndEvent.
    uint64_t BeginEvent(LuaEngine& lua);
    void EndEvent(uint64_t token);
    void ShiftWaitDeadlines(LuaEngine& lua, std::chrono::steady_clock::duration pause);
    std::vector<std::string> StackFiles() const;

private:
    bool Load(const std::vector<uint8_t> &image, const std::string &label);
    bool FindLabel(const std::string &label, size_t *pc);

    AsbScript script_;
    size_t pc_ = 0;
    bool loaded_ = false;
    bool halted_ = false;
    PackManager *packs_ = nullptr;
    std::string current_file_;   // cache: the main loop re-jumps every frame
    size_t file_lines_ = 0;
    size_t file_labels_ = 0;
    struct Frame {
        std::string file;
        size_t pc;
        bool halted = false;
        uint64_t event = 0;
        LuaEngine* lua = nullptr;
        LuaEngine::WaitState wait{};
    };
    std::vector<Frame> callstack_;
    uint64_t next_event_ = 0;
    uint64_t event_entry_ = 0;
    uint64_t event_revision_ = 0;
    uint64_t flow_revision_ = 0;
};

} // namespace artc
