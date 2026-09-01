// pack_manager.h — multi-pack resolution over a base .pfs plus its patch chain.
//
// Behavior spec (from engine docs + samples):
//   * a game ships `xxx.pfs` and optional patch packs `xxx.pfs.000`, `.001`, ...
//   * lookup order is later-pack-wins: a name present in a patch pack resolves
//     there; names missing from patches fall through to the base pack
//   * every pack in the chain shares the same XOR key (title-dependent)
#pragma once
#include "pack/pf8_reader.h"

#include <memory>
#include <string>
#include <vector>

namespace artc {

class PackManager {
public:
    // `base_path` is the main pack; patch packs `base_path.000` and upward are
    // opened while they exist. All packs share `key`.
    bool OpenChain(const std::string &base_path, const std::vector<uint8_t> &key);

    // Resolve a name across the chain (patch packs win). Returns the content.
    bool Read(const std::string &name, std::vector<uint8_t> &out) const;
    bool Exists(const std::string &name) const;

    // First TTF/OTF in the chain (prefers entries under font/); "" when none.
    std::string FindFont() const;

    const std::vector<std::unique_ptr<Pf8Reader>> &Packs() const { return packs_; }
    // Base pack path passed to OpenChain (the primary .pfs / chain base).
    const std::string &PackPath() const { return base_path_; }

private:
    std::vector<std::unique_ptr<Pf8Reader>> packs_;
    std::string base_path_;
};

} // namespace artc
