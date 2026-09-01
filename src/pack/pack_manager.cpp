#include "pack/pack_manager.h"

#include <cstdio>

namespace artc {

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
    return false;
}

bool PackManager::Exists(const std::string &name) const {
    Pf8Entry e;
    for (auto it = packs_.rbegin(); it != packs_.rend(); ++it) {
        if ((*it)->Find(name, e)) return true;
    }
    return false;
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
