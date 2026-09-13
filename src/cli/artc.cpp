// artc.cpp — host CLI test tool for the compat engine.
//
//   artc list   <pack> --key <hex>            list pf8 table entries
//   artc verify <pack> --key <hex>            decrypt-first-file magic sanity check
//   artc extract <pack> <name> [--key <hex>] [-o out]
//   artc ini     <pack> <name> [--key <hex>]  print an ini from the pack
//   artc runlua  <pack> <script> --key <hex> [--os <os>]  boot the Lua bridge
#include "config/ini.h"
#include "log/logger.h"
#include "pack/pack_manager.h"
#include "script/lua_engine.h"
#include "script/asb_parser.h"
#include "script/iet_interpreter.h"
#include "util/byteutil.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace artc;

namespace artc {
int RunDrive(const std::string &pack, const std::string &osName, int frames,
             const std::vector<std::string> &tapSpecs,
             const std::vector<std::string> &asserts);
} // namespace artc

namespace {

int Usage() {
    std::fprintf(stderr,
        "usage:\n"
        "  artc list    <pack> --key <hex>\n"
        "  artc verify  <pack> --key <hex>\n"
        "  artc extract <pack> <name> [--key <hex>] [-o <out>]\n"
        "  artc ini     <pack> <name> [--key <hex>]\n"
        "  artc runlua  <pack> <script> --key <hex> [--os windows]\n"
        "  artc runiet  <pack> <script> --os windows   (auto key)\n"
        "  artc asb     <pack> <name>                  decode a compiled .asb script\n"
        "  artc drive   <pack> [--os android|windows] [--frames N]\n"
        "                       [--tap x,y@frame ...] [--journey title|prologue]\n"
        "                       [--assert SUBSTR ...]  host frame-loop harness\n");
    return 2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 3) return Usage();
    const std::string cmd = argv[1];
    const std::string pack = argv[2];

    std::string key_hex, name, out_path, os_name = "windows";
    bool have_key = false;
    int frames = 3000;
    std::vector<std::string> taps;
    std::vector<std::string> asserts;
    std::string journey;
    for (int i = 3; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--key" && i + 1 < argc) { key_hex = argv[++i]; have_key = true; }
        else if (a == "-o" && i + 1 < argc) out_path = argv[++i];
        else if (a == "--os" && i + 1 < argc) os_name = argv[++i];
        else if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--tap" && i + 1 < argc) { taps.emplace_back(argv[++i]); }
        else if (a == "--assert" && i + 1 < argc) { asserts.emplace_back(argv[++i]); }
        else if (a == "--journey" && i + 1 < argc) { journey = argv[++i]; }
        else name = a;
    }
    // Deterministic journey presets: advance the common flows with periodic
    // centre taps so a real pack can be regression-driven without a human.
    if (!journey.empty()) {
        int end = journey == "title" ? 700 : journey == "prologue" ? 2200 : 0;
        if (end == 0) { std::fprintf(stderr, "unknown journey: %s\n", journey.c_str()); return 2; }
        for (int f = 20; f <= end; f += 60) taps.push_back("640,400@" + std::to_string(f));
    }
    std::vector<uint8_t> key;
    if (have_key && !ParseHexKey(key_hex, key)) {
        std::fprintf(stderr, "bad key hex\n");
        return 2;
    }

    Pf8Reader reader;
    if (!reader.Open(pack, key)) {
        std::fprintf(stderr, "not a parseable pf8 pack: %s\n", pack.c_str());
        return 1;
    }

    if (cmd == "list") {
        for (const Pf8Entry &e : reader.Entries())
            std::printf("%-64s off=0x%08llx size=%u\n", e.name.c_str(),
                        static_cast<unsigned long long>(e.offset), e.size);
        std::fprintf(stderr, "total: %u files\n", reader.FileCount());
        return 0;
    }

    if (cmd == "verify") {
        // sanity: decrypt the first entries and print their leading bytes
        int n = 0;
        for (const Pf8Entry &e : reader.Entries()) {
            if (n++ >= 4) break;
            std::vector<uint8_t> out;
            if (!reader.Read(e, out)) continue;
            std::printf("%-48s %s\n", e.name.c_str(), ToHex(out.data(), 8).c_str());
        }
        return 0;
    }

    if (cmd == "extract") {
        if (name.empty()) return Usage();
        std::vector<uint8_t> out;
        if (!reader.Read(name, out)) {
            std::fprintf(stderr, "extract failed: %s\n", name.c_str());
            return 1;
        }
        const std::string dest = out_path.empty() ? NormalizePackName(name) : out_path;
        FILE *fp = std::fopen(dest.c_str(), "wb");
        if (!fp) return 1;
        std::fwrite(out.data(), 1, out.size(), fp);
        std::fclose(fp);
        std::printf("extracted %zu B -> %s\n", out.size(), dest.c_str());
        return 0;
    }

    if (cmd == "ini") {
        if (name.empty()) return Usage();
        std::vector<uint8_t> out;
        if (!reader.Read(name, out)) return 1;
        std::fwrite(out.data(), 1, out.size(), stdout);
        return 0;
    }

    if (cmd == "asb") {
        if (name.empty()) return Usage();
        Pf8Reader reader2;
        if (!reader2.Open(pack, key)) return 1;
        Pf8Entry e;
        if (!reader2.Find(name, e)) { std::fprintf(stderr, "not found: %s\n", name.c_str()); return 1; }
        std::vector<uint8_t> img;
        if (!reader2.Read(e, img)) return 1;
        AsbScript script;
        if (!ParseAsb(img, &script)) { std::fprintf(stderr, "asb parse failed\n"); return 1; }
        for (const AsbLine &ln : script.lines) {
            if (ln.is_label) { std::printf("*%s\n", ln.command.c_str()); continue; }
            std::printf("[%s", ln.command.c_str());
            for (const auto &kv : ln.attrs)
                std::printf(" %s=\"%s\"", kv.first.c_str(), kv.second.c_str());
            std::printf("]\n");
        }
        return 0;
    }

    if (cmd == "drive") {
        return RunDrive(pack, os_name, frames, taps, asserts);
    }

    if (cmd == "runlua" || cmd == "runiet") {
        if (name.empty()) return Usage();
        const bool do_call = (os_name == "call");
        PackManager packs;
        if (!packs.OpenChain(pack, key)) return 1;
        std::printf("packs loaded: %zu\n", packs.Packs().size());

        Ini ini;
        std::vector<uint8_t> ini_bytes;
        if (packs.Read("system.ini", ini_bytes))
            ini.Parse(std::string(ini_bytes.begin(), ini_bytes.end()));

        LuaEngine lua;
        if (!lua.Init(&packs, ini, os_name, ini.GetInt("ANDROID", "WIDTH", 1280),
                      ini.GetInt("ANDROID", "HEIGHT", 720)))
            return 1;
        std::string err;
        if (cmd == "runlua" && !lua.RunPackScript(name, &err)) {
            std::fprintf(stderr, "lua error: %s\n", err.c_str());
            return 1;
        }
        if (cmd == "runlua")
            std::printf("lua script finished: %s\n", name.c_str());
        if (cmd == "runiet") {
            IetRunner iet(&packs, &lua);
            if (!iet.Run(name)) return 1;
            std::printf("iet finished: %s (stopped=%d)\n", name.c_str(),
                        iet.Stopped() ? 1 : 0);
        }
        return 0;
    }

    return Usage();
}
