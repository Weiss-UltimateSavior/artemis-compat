#include "pack/pf8_reader.h"
#include "pack/sha1.h"
#include "util/byteutil.h"

#include <cstdio>
#include <cstring>

namespace artc {

bool Pf8Reader::Open(const std::string &path, const std::vector<uint8_t> &key) {
    path_ = path;
    key_ = key;
    entries_.clear();

    FILE *fp = std::fopen(path.c_str(), "rb");
    if (!fp) return false;

    uint8_t header[11];
    if (std::fread(header, 1, sizeof(header), fp) != sizeof(header) ||
        std::memcmp(header, "pf", 2) != 0 || header[2] != '8') {
        std::fclose(fp);
        return false;
    }
    auto rd32 = [&](const uint8_t *p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    const uint32_t index_size = rd32(header + 3);
    const uint32_t file_count = rd32(header + 7);

    // Auto key derivation: SHA1 over the hashed region [7, 7 + index_size).
    if (key_.empty()) {
        std::vector<uint8_t> hashed(index_size);
        if (std::fseek(fp, 7, SEEK_SET) != 0 ||
            std::fread(hashed.data(), 1, hashed.size(), fp) != hashed.size()) {
            std::fclose(fp);
            return false;
        }
        key_.resize(20);
        Sha1(hashed.data(), hashed.size(), key_.data());
    }

    // Record table: [11, 7 + index_size)
    const long table_bytes = static_cast<long>(index_size) - 4;
    if (table_bytes <= 0) {
        std::fclose(fp);
        return false;
    }
    std::vector<uint8_t> table(table_bytes);
    if (std::fseek(fp, 11, SEEK_SET) != 0 ||
        std::fread(table.data(), 1, table.size(), fp) != table.size()) {
        std::fclose(fp);
        return false;
    }
    std::fclose(fp);

    entries_.reserve(file_count);
    size_t pos = 0;
    for (uint32_t n = 0; n < file_count && pos + 4 <= table.size(); ++n) {
        const uint32_t name_len = static_cast<uint32_t>(table[pos]) |
                                 (static_cast<uint32_t>(table[pos + 1]) << 8) |
                                 (static_cast<uint32_t>(table[pos + 2]) << 16) |
                                 (static_cast<uint32_t>(table[pos + 3]) << 24);
        pos += 4;
        if (pos + name_len + 12 > table.size()) break;
        Pf8Entry e;
        e.name.assign(reinterpret_cast<const char *>(table.data()) + pos, name_len);
        pos += name_len;
        auto rd = [&]() -> uint32_t {
            const uint32_t v = static_cast<uint32_t>(table[pos]) |
                              (static_cast<uint32_t>(table[pos + 1]) << 8) |
                              (static_cast<uint32_t>(table[pos + 2]) << 16) |
                              (static_cast<uint32_t>(table[pos + 3]) << 24);
            pos += 4;
            return v;
        };
        (void)rd(); // pad
        e.offset = rd();
        e.size = rd();
        entries_.push_back(std::move(e));
    }
    return !entries_.empty();
}

bool Pf8Reader::Find(const std::string &name, Pf8Entry &out) const {
    for (const Pf8Entry &e : entries_) {
        if (e.name == name) { out = e; return true; }
    }
    const std::string want = NormalizePackName(name);
    for (const Pf8Entry &e : entries_) {
        if (NormalizePackName(e.name) == want) { out = e; return true; }
    }
    return false;
}

void Pf8Reader::Decrypt(uint8_t *data, size_t len) const {
    if (key_.empty()) return; // passthrough when no key configured
    // Behavior note: the XOR phase restarts at each file's data start
    // (verified: files deep inside packs decrypt cleanly from phase 0).
    for (size_t i = 0; i < len; ++i)
        data[i] ^= key_[i % key_.size()];
}

bool Pf8Reader::Read(const Pf8Entry &e, std::vector<uint8_t> &out) const {
    if (e.offset == 0 || e.size == 0) { out.clear(); return true; } // empty entry
    FILE *fp = std::fopen(path_.c_str(), "rb");
    if (!fp) return false;
    if (std::fseek(fp, static_cast<long>(e.offset), SEEK_SET) != 0) {
        std::fclose(fp);
        return false;
    }
    out.resize(e.size);
    const size_t got = std::fread(out.data(), 1, e.size, fp);
    std::fclose(fp);
    if (got != e.size) return false;
    Decrypt(out.data(), out.size());
    return true;
}

bool Pf8Reader::Read(const std::string &name, std::vector<uint8_t> &out) const {
    Pf8Entry e;
    if (!Find(name, e)) return false;
    return Read(e, out);
}

} // namespace artc
