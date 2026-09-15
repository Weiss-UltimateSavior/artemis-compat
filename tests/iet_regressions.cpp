// iet_regressions.cpp — .iet line model + linear interpreter + path parity.
//
// Covers the two .iet execution paths: IetRunner (linear, used for the
// framework's system/first.iet) and AsbRunner (the same text parsed into the
// control-flow line model). For a script without branching both must dispatch
// the exact same tag sequence. Fixtures are generated text only.

#include "config/ini.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "pack/sha1.h"
#include "script/asb_parser.h"
#include "script/iet_interpreter.h"
#include "script/lua_engine.h"
#include "script/preprocess.h"

#include <cstdint>
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

struct FileSpec { std::string name; std::string data; };
std::vector<uint8_t> BuildPack(const std::vector<FileSpec> &files) {
    std::vector<uint8_t> records;
    for (const auto &f : files) {
        Put32(records, (uint32_t)f.name.size());
        records.insert(records.end(), f.name.begin(), f.name.end());
        Put32(records, 0);
        Put32(records, 0);
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
};

// A script with no control flow: both interpreters must produce this order.
const char *kLinearScript =
    "// synthetic boot script\n"
    "*top\n"
    "[lua]\nlocal x = 1 + 1\n[/lua]\n"
    "[ietOne a=\"1\" b=2]\n"
    "some scenario text line\n"
    "[ietTwo]\n"
    "[stop]\n";

std::vector<std::string> OnlyIetSteps(const std::vector<std::string> &steps) {
    std::vector<std::string> out;
    for (const auto &s : steps)
        if (s.rfind("iet", 0) == 0) out.push_back(s);
    return out;
}

} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
                         ("artc_iet_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
    const std::string pack_path = (dir / "root.pfs").string();
    {
        std::vector<uint8_t> bytes = BuildPack({
            {"system.ini", "[ANDROID]\nWIDTH=1280\nHEIGHT=720\n"},
            {"system/first.iet", kLinearScript},
            {"linear.iet", kLinearScript},
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

    // ---- 1) .iet line model -------------------------------------------------
    {
        artc::AsbScript script;
        Check(artc::ParseIetScript(artc::PreprocessScript(kLinearScript), &script),
              "parse synthetic .iet");
        bool has_top = false, has_lua = false, has_one = false, has_two = false;
        bool attrs_ok = false, text_ok = false;
        for (const auto &ln : script.lines) {
            if (ln.is_label && ln.command == "top") has_top = true;
            if (ln.command == "\x02LUA") has_lua = true;
            if (ln.command == "ietOne") {
                has_one = true;
                std::string a, b;
                for (const auto &kv : ln.attrs) {
                    if (kv.first == "a") a = kv.second;
                    if (kv.first == "b") b = kv.second;
                }
                attrs_ok = a == "1" && b == "2";
            }
            if (ln.command == "ietTwo") has_two = true;
            if (ln.command == "\x01TEXT") {
                for (const auto &kv : ln.attrs)
                    if (kv.first == "text" && kv.second == "some scenario text line")
                        text_ok = true;
            }
        }
        Check(has_top, "label recorded");
        Check(has_lua, "[lua] chunk becomes a LUA command line");
        Check(has_one && has_two, "bracket tags parsed");
        Check(attrs_ok, "quoted and bare attributes decoded");
        Check(text_ok, "plain text lines preserved");
        bool found_label = false;
        for (const auto &kv : script.labels)
            if (kv.first == "top") found_label = true;
        Check(found_label, "label index built");
    }

    // ---- 2) linear path (IetRunner) ----------------------------------------
    std::vector<std::string> linear_steps;
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (linear)");
        artc::IetRunner iet(&packs, &lua);
        Check(iet.Run("system/first.iet"), "linear run");
        Check(iet.Stopped(), "linear run honours [stop]");
        artc::SetLogSink(nullptr);
        linear_steps = OnlyIetSteps(trace.steps());
        const std::vector<std::string> want{"ietOne", "ietTwo"};
        Check(linear_steps == want, "linear path dispatches tags in order");
    }

    // ---- 3) control-flow path (AsbRunner) on the same script ---------------
    {
        Trace trace;
        artc::SetLogSink([&trace](int, const std::string &m) { trace.lines.push_back(m); });
        artc::LuaEngine lua;
        Check(lua.Init(&packs, ini, "android", 1280, 720), "lua init (asb path)");
        artc::AsbRunner runner;
        runner.SetPackSource(&packs);
        Check(runner.Jump("linear.iet", "top"), "jump to the linear script");
        for (int i = 0; i < 200 && runner.Loaded() && !runner.Halted(); ++i)
            runner.ExecuteLine(lua);
        artc::SetLogSink(nullptr);
        const auto asb_steps = OnlyIetSteps(trace.steps());
        Check(asb_steps == linear_steps,
              "both .iet paths dispatch the same tag sequence");
    }

    fs::remove_all(dir);
    if (g_failures) {
        std::cerr << g_failures << " iet regression failure(s)\n";
        return 1;
    }
    std::cout << "iet regressions passed\n";
    return 0;
}
