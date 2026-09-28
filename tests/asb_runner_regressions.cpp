// asb_runner_regressions.cpp — native script runner state machine.
//
// Fixtures are synthetic .iet text scripts packed into a generated pf8 (same
// writer as pf8_regressions) plus a minimal LuaEngine. Tag dispatch on an
// unregistered tag is a no-op that emits the first-seen `tag[trace]` line, so
// the *order* of unique step tags is the execution trace — no game assets,
// no GL.
//
// Covered: linear+jump, 3-deep nested call/return, cross-file call/return,
// the [return]→call resume point (pc_pending_), event-frame resume, DiscardFlow.

#include "config/ini.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "pack/sha1.h"
#include "script/asb_parser.h"
#include "script/lua_engine.h"

#include <cstdint>
#include <cstdio>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}

void Put32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back(v & 0xFF);
    out.push_back((v >> 8) & 0xFF);
    out.push_back((v >> 16) & 0xFF);
    out.push_back((v >> 24) & 0xFF);
}

// Encrypted pf8 writer (payload XOR'd with SHA1(file_count || records)), the
// same container form the reader derives keys for.
struct FileSpec { std::string name; std::string data; };
std::vector<uint8_t> BuildPack(const std::vector<FileSpec> &files) {
    std::vector<uint8_t> records;
    for (const auto &f : files) {
        Put32(records, (uint32_t)f.name.size());
        records.insert(records.end(), f.name.begin(), f.name.end());
        Put32(records, 0);
        Put32(records, 0);   // data offset placeholder
        Put32(records, (uint32_t)f.data.size());
    }
    const uint32_t index_size = (uint32_t)records.size() + 4;
    const uint32_t data_start = 7 + index_size;
    std::vector<uint8_t> data;
    size_t record_pos = 0;
    for (const auto &f : files) {
        const uint32_t off = data_start + (uint32_t)data.size();
        const size_t field = record_pos + 4 + f.name.size() + 4;
        records[field + 0] = off & 0xFF;
        records[field + 1] = (off >> 8) & 0xFF;
        records[field + 2] = (off >> 16) & 0xFF;
        records[field + 3] = (off >> 24) & 0xFF;
        data.insert(data.end(), f.data.begin(), f.data.end());
        record_pos += 4 + f.name.size() + 12;
    }
    std::vector<uint8_t> hashed;
    Put32(hashed, (uint32_t)files.size());
    hashed.insert(hashed.end(), records.begin(), records.end());
    std::vector<uint8_t> key(20);
    artc::Sha1(hashed.data(), hashed.size(), key.data());
    size_t offset = 0;
    for (const auto &f : files) {
        for (size_t i = 0; i < f.data.size(); ++i)
            data[offset + i] ^= key[i % key.size()];
        offset += f.data.size();
    }
    std::vector<uint8_t> out;
    out.push_back('p'); out.push_back('f'); out.push_back('8');
    Put32(out, index_size);
    Put32(out, (uint32_t)files.size());
    out.insert(out.end(), records.begin(), records.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// Captures engine logs and exposes the first-seen step-tag order as the trace.
struct Trace {
    std::vector<std::string> lines;
    std::vector<std::string> steps() const {
        std::vector<std::string> out;
        for (const auto &l : lines) {
            const size_t p = l.find("tag[trace]: ");
            if (p == std::string::npos) continue;
            std::string t = l.substr(p + 12);
            const size_t e = t.find(' ');
            if (e != std::string::npos) t = t.substr(0, e);
            out.push_back(t);
        }
        return out;
    }
    bool has(const std::string &needle) const {
        for (const auto &l : lines)
            if (l.find(needle) != std::string::npos) return true;
        return false;
    }
    size_t count(const std::string &needle) const {
        size_t n = 0;
        for (const auto &l : lines)
            if (l.find(needle) != std::string::npos) ++n;
        return n;
    }
};

std::vector<std::string> OnlyStepsWithPrefix(const std::vector<std::string> &steps,
                                             const std::string &prefix) {
    std::vector<std::string> out;
    for (const auto &s : steps)
        if (s.rfind(prefix, 0) == 0) out.push_back(s);
    return out;
}

void Run(artc::AsbRunner &runner, artc::LuaEngine &lua, int max_steps = 500) {
    for (int i = 0; i < max_steps && runner.Loaded() && !runner.Halted(); ++i)
        runner.ExecuteLine(lua);
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("artc_asb_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string pack_path = (dir / "root.pfs").string();
    {
        std::vector<uint8_t> bytes = BuildPack({
            {"system.ini", "[ANDROID]\nWIDTH=1280\nHEIGHT=720\n"},
            // linear + 3-deep nested call/return (jump skips dead code)
            {"main.iet",
             "*start\n[step1]\n[jump label=\"mid\"]\n[stepDead]\n"
             "*mid\n[call label=\"a\"]\n[stepF]\n[return]\n"
             "*a\n[stepA]\n[call label=\"b\"]\n[stepE]\n[return]\n"
             "*b\n[stepB]\n[call label=\"c\"]\n[stepD]\n[return]\n"
             "*c\n[stepC]\n[return]\n"},
            // cross-file call/return
            {"xmain.iet", "*start\n[stepX1]\n[call file=\"xsub.iet\" label=\"s\"]\n[stepX3]\n[return]\n"},
            {"xsub.iet", "*s\n[stepX2]\n[return]\n"},
            // [return] then a call issued before the resumed line runs
            {"pmain.iet",
             "*start\n[stepP1]\n[call label=\"sub\"]\n[stepP3]\n[return]\n"
             "*sub\n[stepP2]\n[return]\n"
             "*other\n[stepP4]\n[return]\n"},
            // event frame: BeginEvent/EndEvent around an interrupt
            {"emain.iet", "*start\n[stepE1]\n[stepE2]\n[return]\n"},
            // omitted-file jump targets a label in the current file (official
            // same-file jump semantics); stepDead must be skipped
            {"same.iet",
             "*start\n[stepS1]\n[jump label=\"target\"]\n[stepDead]\n"
             "*target\n[stepS2]\n[return]\n"},
            // a boot macro script that defines a global entry label
            {"macro_x.iet", "*game_start\n[stepG1]\n[stepG2]\n[return]\n"},
            // a different boot script whose cursor has no game_start label;
            // the bare jump must resolve to macro_x.iet (cross-file index)
            {"boot_x.iet", "*start\n[stepB1]\n[jump label=\"game_start\"]\n[stepDead]\n"},
            // a top-level [lua] block defining a global sits ABOVE the entry
            // label; the label-gated flow then calllua's that global.
            {"luaent.iet",
             "*top\n[lua]\nlua_init_count = (lua_init_count or 0) + 1\n"
             "function lua_global_fn() e:tag{\"luaDef\"} end\n[/lua]\n"
             "*entry\n[calllua function=\"lua_global_fn\"]\n[stepLU]\n[return]\n"},
            {"queue_init.iet",
             "[lua]\nfunction queue_ready() e:tag{\"stepQueueReady\"} end\n[/lua]\n"
             "[stepQueueInit]\n[return]\n"},
            {"queue_after.iet",
             "*entry\n[calllua function=\"queue_ready\"]\n[stepQueueAfter]\n[stop]\n"},
        });
        std::ofstream out(pack_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(bytes.data()), (std::streamsize)bytes.size());
    }

    artc::PackManager packs;
    Check(packs.OpenChain(pack_path, {}), "open synthetic pack");
    artc::Ini ini;
    std::vector<uint8_t> ini_bytes;
    if (packs.Read("system.ini", ini_bytes))
        ini.Parse(std::string(ini_bytes.begin(), ini_bytes.end()));

    // ---- 1) linear + jump + 3-deep nesting --------------------------------
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("main.iet", "start"), "jump to main:start");
        Run(runner, lua);
        artc::SetLogSink(nullptr);
        const auto steps = OnlyStepsWithPrefix(trace.steps(), "step");
        const std::vector<std::string> want{"step1", "stepA", "stepB", "stepC",
                                            "stepD", "stepE", "stepF"};
        Check(steps == want, "nested call/return executes in order");
        Check(!trace.has("stepDead"), "jump skips intermediate code");
    }

    // ---- 2) cross-file call/return ----------------------------------------
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (cross-file)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("xmain.iet", "start"), "jump to xmain:start");
        Run(runner, lua);
        artc::SetLogSink(nullptr);
        const auto steps = OnlyStepsWithPrefix(trace.steps(), "step");
        const std::vector<std::string> want{"stepX1", "stepX2", "stepX3"};
        Check(steps == want, "cross-file call returns to the caller");
        Check(trace.has("asb: return to xmain.iet"), "return log names the caller");
    }

    // ---- 3) resume point after [return] (pc_pending_) ---------------------
    // After sub's [return] the runner sits at "[stepP3]" without having run it;
    // a call issued at that moment must resume *at* [stepP3], not after it.
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (resume)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("pmain.iet", "start"), "jump to pmain:start");
        // Run into sub until [stepP2] dispatched (robust to label lines)...
        for (int i = 0; i < 50 && runner.Loaded() && !runner.Halted(); ++i) {
            runner.ExecuteLine(lua);
            if (trace.has("stepP2")) break;
        }
        // ...then one more line: sub's [return] resumes at [stepP3] with the
        // line still pending (this is the case the pc_pending_ fix covers).
        runner.ExecuteLine(lua);
        Check(!runner.Halted(), "runner still live after sub return");
        Check(runner.Call("pmain.iet", "other"), "call while the resume line is pending");
        runner.ExecuteLine(lua);                       // [stepP4]
        runner.ExecuteLine(lua);                       // [return] -> resume @2
        Run(runner, lua);
        artc::SetLogSink(nullptr);
        const auto steps = OnlyStepsWithPrefix(trace.steps(), "step");
        const std::vector<std::string> want{"stepP1", "stepP2", "stepP4", "stepP3"};
        Check(steps == want, "pending resume line executes after the nested call");
    }

    // ---- 4) event frame resume --------------------------------------------
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (event)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("emain.iet", "start"), "jump to emain:start");
        runner.ExecuteLine(lua);                       // [stepE1]
        const uint64_t token = runner.BeginEvent(lua);
        Check(token != 0, "event frame opened");
        runner.EndEvent(token);                        // no control transfer: pop
        Run(runner, lua);
        artc::SetLogSink(nullptr);
        const auto steps = OnlyStepsWithPrefix(trace.steps(), "step");
        const std::vector<std::string> want{"stepE1", "stepE2"};
        Check(steps == want, "event with no transfer resumes the same line");
    }

    // ---- 5) DiscardFlow + re-entry ----------------------------------------
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (discard)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("emain.iet", "start"), "jump before discard");
        runner.ExecuteLine(lua);
        runner.DiscardFlow();
        Check(!runner.Loaded() && runner.Halted(), "discard unloads the flow");
        Check(runner.Jump("emain.iet", "start"), "re-entry after discard");
        Run(runner, lua);
        artc::SetLogSink(nullptr);
        Check(trace.count("asb: load emain.iet") >= 2, "script re-parsed on re-entry");
    }

    // ---- 6) omitted-file jump: in-file then cross-file label resolution ----
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (bare jump)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);

        // (a) same-file omitted-file jump lands on the in-file label.
        Check(runner.Jump("same.iet", "start"), "load same.iet");
        runner.ExecuteLine(lua);  // *start
        runner.ExecuteLine(lua);  // [stepS1]
        Check(runner.Jump("", "target"), "in-file bare jump accepted");
        Run(runner, lua);

        // (b) cross-file bare label: preload the macro defining game_start,
        // then put the cursor in a different script that lacks it.
        Check(runner.Jump("macro_x.iet", "start"), "preload macro (labels indexed)");
        Check(runner.Jump("boot_x.iet", "start"), "load boot_x.iet");
        runner.ExecuteLine(lua);  // *start
        runner.ExecuteLine(lua);  // [stepB1]
        Check(runner.Jump("", "game_start"), "cross-file bare jump accepted");
        Check(!runner.Halted(), "runner live after cross-file bare jump");
        Run(runner, lua);
        artc::SetLogSink(nullptr);

        const auto steps = OnlyStepsWithPrefix(trace.steps(), "step");
        Check(steps == std::vector<std::string>{"stepS1", "stepS2", "stepB1",
                                                "stepG1", "stepG2"},
              "bare jumps resolve in-file then cross-file");
        Check(trace.has("bare label game_start -> macro_x.iet"),
              "cross-file resolution logged");

        // (c) a context-less placeholder (nothing loaded) stays safe.
        artc::AsbRunner empty;
        empty.SetPackSource(&packs);
        Check(empty.Jump("", "x") && empty.Call("", "x"), "context-less empty target safe");
        Check(!trace.has("script not found in packs: "), "no missing-script read");
    }

    // ---- 7) text script load executes all top-level Lua initialization ----
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (load-time blocks)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        runner.SetLuaEngine(&lua);
        // Enter at a later label; library globals must be available before
        // the label-gated flow reaches calllua.
        Check(runner.Jump("luaent.iet", "entry"), "load luaent.iet at entry");
        Run(runner, lua);
        Check(lua.DoString("assert(lua_init_count == 1)", "initializer count"),
              "entry label initializes the script once");
        Check(runner.Jump("luaent.iet", "top"), "revisit text script from its start");
        Run(runner, lua);
        Check(lua.DoString("assert(lua_init_count == 1)", "initializer count after traversal"),
              "normal traversal does not execute load-time Lua again");
        artc::SetLogSink(nullptr);
        Check(trace.has("luaDef") && trace.has("stepLU"),
              "load-time Lua definitions run before the entry-label flow");
    }

    // ---- 8) loading queued libraries initializes Lua before the entry jump --
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (boot queue)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        runner.SetLuaEngine(&lua);
        lua.EnqueueTag("call", {{"file", "queue_init.iet"}});
        lua.EnqueueTag("jump", {{"file", "queue_after.iet"}, {"label", "entry"}});
        auto drain = [&]() {
            while (!lua.IsWaiting() && lua.HasQueuedTag()) {
                std::string name, file, label;
                std::vector<std::pair<std::string, std::string>> attrs;
                if (!lua.PopQueuedTag(&name, &attrs)) break;
                for (const auto& kv : attrs) {
                    if (kv.first == "file") file = kv.second;
                    if (kv.first == "label") label = kv.second;
                }
                if (name == "call") runner.Call(file, label);
                else if (name == "jump") runner.Jump(file, label);
                else lua.DispatchTag(name, attrs, false);
            }
        };
        for (int i = 0; i < 100; ++i) {
            drain();
            if (runner.Loaded() && !runner.Halted() && !lua.IsWaiting()) runner.ExecuteLine(lua);
        }
        artc::SetLogSink(nullptr);
        Check(OnlyStepsWithPrefix(trace.steps(), "stepQueue") ==
                  std::vector<std::string>{"stepQueueReady", "stepQueueAfter"},
              "queued loads initialize Lua without executing skipped tag bodies");
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::cerr << g_failures << " asb runner regression failure(s)\n";
        return 1;
    }
    std::cout << "asb runner regressions passed\n";
    return 0;
}
