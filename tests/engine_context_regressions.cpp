#include "core/engine_context.h"
#include "script/lua_engine.h"
#include "pack/pack_manager.h"
#include "pack/sha1.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
namespace fs = std::filesystem;
namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << "\n"; }
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

}
int main() {
    const auto root = fs::temp_directory_path() / ("artc_context_" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto a = root / "a", b = root / "b", saves = root / "private-saves";
    for (const auto &dir : {a, b, saves}) fs::create_directories(dir);
    auto write = [](const fs::path &dir, std::vector<FileSpec> files) {
        const auto bytes = BuildPack(files);
        std::ofstream out(dir / "root.pfs", std::ios::binary);
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
    };
    write(a, {{"system.ini", "[ANDROID]\nWIDTH=640\nHEIGHT=480\n"},
              {"system/first.iet", "[lua]\ncontext_marker=17\n[/lua]\n"}});
    write(b, {{"system/first.iet", "[lua]\ncontext_marker=23\n[/lua]\n"}});
    artc::EngineContext ctx;
    Check(!ctx.Start(false), "start before open is rejected");
    Check(!ctx.BootFramework(), "boot before start is rejected");
    Check(!ctx.Open((root / "missing.pfs").string(), "android"), "failed pack open");
    Check(!ctx.Opened(), "failed open leaves no pack");
    Check(ctx.Open(a.string(), "android"), "open default layout");
    Check(ctx.saveDir() == a.string(), "default saves next to pack");
    Check(ctx.stageW() == 640 && ctx.stageH() == 480, "read initial stage");
    Check(ctx.Start(false) && ctx.BootFramework(), "boot initial session");
    Check(ctx.lua().SaveDir() == a.string(), "Lua receives default save directory");
    auto *audio = &ctx.audio();
    ctx.ResetSession();
    Check(ctx.Opened() && !ctx.Started(), "reset keeps packs and drops session");
    Check(ctx.Start(false) && ctx.BootFramework(), "boot reset session");
    Check(&ctx.audio() == audio, "audio remains owned by context across reset");
    artc::EngineContext::SetCurrent(&ctx);
    ctx.Shutdown();
    ctx.Shutdown();
    Check(!ctx.Opened() && !ctx.Started(), "shutdown is idempotent");
    Check(artc::EngineContext::Current() == nullptr, "shutdown removes registry entry");
    Check(ctx.Open(b.string(), "ohos", {}, saves.string()), "reopen another game");
    Check(ctx.stageW() == 1280 && ctx.stageH() == 720, "new game does not inherit old ini");
    Check(ctx.Start(false) && ctx.BootFramework(), "boot with explicit save directory");
    Check(ctx.lua().SaveDir() == saves.string(), "Lua receives explicit save directory");
    Check(ctx.lua().SaveSystemData(), "write system data in selected directory");
    Check(fs::exists(saves / "system.dat"), "system data written outside game directory");
    Check(!fs::exists(b / "system.dat"), "explicit save path leaves game directory untouched");
    ctx.Shutdown();
    fs::remove_all(root);
    return failures ? 1 : 0;
}
