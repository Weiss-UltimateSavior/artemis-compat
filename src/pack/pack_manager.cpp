#include "pack/pack_manager.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace artc {
namespace {
std::filesystem::path LoosePath(const std::string& base, std::string name) {
    std::replace(name.begin(),name.end(),'\\','/');
    const auto relative=std::filesystem::path(name).lexically_normal();
    if(base.empty() || relative.empty() || relative.is_absolute()) return {};
    for(const auto& part:relative) if(part=="..") return {};
    return std::filesystem::path(base).parent_path()/relative;
}
}

bool PackManager::OpenChain(const std::string &base_path,
                            const std::vector<uint8_t> &key) {
    packs_.clear();
    base_path_ = base_path;
    auto base = std::make_unique<Pf8Reader>();
    if (!base->Open(base_path, key)) return false;
    packs_.push_back(std::move(base));

    for (uint32_t idx = 0;; ++idx) {
        char suffix[16];
        std::snprintf(suffix, sizeof(suffix), ".%03u", idx);
        const std::string patch_path = base_path + suffix;
        auto patch = std::make_unique<Pf8Reader>();
        if (!patch->Open(patch_path, key)) break; // chain ends at first missing pack
        packs_.push_back(std::move(patch));
    }
    return true;
}

bool PackManager::Read(const std::string &name, std::vector<uint8_t> &out) const {
    // later-pack-wins: walk the chain backwards
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) {
        if ((*it)->Read(name, out)) return true;
    }
    // Games commonly distribute movies beside the PFS rather than inside it.
    const auto path=LoosePath(base_path_,name);
    if(path.empty()) return false;
    std::ifstream input(path,std::ios::binary|std::ios::ate);
    if(!input) return false;
    const auto size=input.tellg();
    if(size<0 || static_cast<uint64_t>(size)>0x7fffffff) return false;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    input.seekg(0);
    if(size && !input.read(reinterpret_cast<char*>(bytes.data()),size)) return false;
    out=std::move(bytes);
    return true;
}

bool PackManager::Exists(const std::string &name) const {
    Pf8Entry e;
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) {
        if ((*it)->Find(name, e)) return true;
    }
    const auto path=LoosePath(base_path_,name);
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path,error);
}

std::string PackManager::FindFont() const {
    std::string fallback;
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) {
        for (const Pf8Entry &e : (*it)->Entries()) {
            const size_t dot = e.name.rfind('.');
            if (dot == std::string::npos) continue;
            const std::string ext = e.name.substr(dot);
            if (ext != ".ttf" && ext != ".otf") continue;
            if (e.name.find("font/") != std::string::npos ||
                e.name.find("font\\") != std::string::npos) {
                return e.name; // preferred: pack font directory
            }
            if (fallback.empty()) fallback = e.name;
        }
    }
    return fallback;
}

} // namespace artc
