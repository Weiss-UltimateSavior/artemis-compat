#include "pack/pf8_reader.h"
#include "pack/sha1.h"
#include "util/byteutil.h"
#include "util/encoding.h"

#include <cstdio>
#include <cstring>
#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace artc {

Pf8Reader::~Pf8Reader() { Close(); }

void Pf8Reader::Close() {
#if !defined(_WIN32)
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
#endif
    entries_.clear();
    index_.clear();
}

// Positional read off the retained pack handle. POSIX keeps one fd for the
// reader lifetime and uses pread (explicit offsets → safe for concurrent
// reads, no per-call fopen); Windows falls back to the legacy open-seek-
// read-close path.
bool Pf8Reader::ReadAt(uint64_t offset, void *buf, size_t len) const {
    if (len == 0) return true;
#if !defined(_WIN32)
    if (fd_ < 0) return false;
    auto *p = static_cast<uint8_t *>(buf);
    size_t done = 0;
    while (done < len) {
        const ssize_t n = ::pread(fd_, p + done, len - done,
                                  static_cast<off_t>(offset + done));
        if (n <= 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
#else
    FILE *fp = std::fopen(path_.c_str(), "rb");
    if (!fp) return false;
    if (std::fseek(fp, static_cast<long>(offset), SEEK_SET) != 0) {
        std::fclose(fp);
        return false;
    }
    const size_t got = std::fread(buf, 1, len, fp);
    std::fclose(fp);
    return got == len;
#endif
}

bool Pf8Reader::Open(const std::string &path, const std::vector<uint8_t> &key) {
    Close();
    path_ = path;
    key_ = key;
    encrypted_ = true;

#if !defined(_WIN32)
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) return false;
#endif

    uint8_t header[11];
    if (!ReadAt(0, header, sizeof(header)) ||
        std::memcmp(header, "pf", 2) != 0) {
        Close();
        return false;
    }
    // '8' is the encrypted generation; '2'/'6' are the PF6-era layout, which
    // stores data in the clear but keeps the same record table.
    const char version = static_cast<char>(header[2]);
    if (version != '8' && version != '2' && version != '6') {
        Close();
        return false;
    }
    encrypted_ = (version == '8');
    if (!encrypted_) key_.clear(); // never XOR a clear-text container
    auto rd32 = [&](const uint8_t *p) -> uint32_t {
        return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
               (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    };
    const uint32_t index_size = rd32(header + 3);
    const uint32_t file_count = rd32(header + 7);

    // Auto key derivation: SHA1 over the hashed region [7, 7 + index_size).
    if (encrypted_ && key_.empty()) {
        std::vector<uint8_t> hashed(index_size);
        if (!ReadAt(7, hashed.data(), hashed.size())) {
            Close();
            return false;
        }
        key_.resize(20);
        Sha1(hashed.data(), hashed.size(), key_.data());
    }

    // Record table: [11, 7 + index_size)
    const long table_bytes = static_cast<long>(index_size) - 4;
    if (table_bytes <= 0) {
        Close();
        return false;
    }
    std::vector<uint8_t> table(table_bytes);
    if (!ReadAt(11, table.data(), table.size())) {
        Close();
        return false;
    }

    entries_.reserve(file_count);
    index_.reserve(file_count);
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
        // First record wins (original engine behavior): emplace does not
        // overwrite an existing key, keeping the earliest entry visible.
        index_.emplace(NormalizeLookupKey(e.name), entries_.size());
        entries_.push_back(std::move(e));
    }
    return !entries_.empty();
}

bool Pf8Reader::Find(const std::string &name, Pf8Entry &out) const {
    // Case-insensitive, first record wins (original engine behavior).
    const std::string want = NormalizeLookupKey(name);
    const auto it = index_.find(want);
    if (it != index_.end()) { out = entries_[it->second]; return true; }
    // Shift_JIS fallback: a UTF-8 query against a CP932-named container.
    std::string cp932;
    if (Utf8ToShiftJis(name, cp932)) {
        const std::string want2 = NormalizeLookupKey(cp932);
        if (want2 != want) {
            const auto it2 = index_.find(want2);
            if (it2 != index_.end()) { out = entries_[it2->second]; return true; }
        }
    }
    return false;
}

void Pf8Reader::DecryptRange(uint8_t *data, size_t len, uint64_t offset) const {
    if (key_.empty()) return; // passthrough when no key configured
    // Behavior note: the XOR phase restarts at each file's data start
    // (verified: files deep inside packs decrypt cleanly from phase 0).
    // A range read starts mid-stream, so begin at the matching phase.
    const size_t phase = static_cast<size_t>(offset % key_.size());
    for (size_t i = 0; i < len; ++i)
        data[i] ^= key_[(phase + i) % key_.size()];
}

bool Pf8Reader::Read(const Pf8Entry &e, std::vector<uint8_t> &out) const {
    if (e.offset == 0 || e.size == 0) { out.clear(); return true; } // empty entry
    out.resize(e.size);
    if (!ReadAt(e.offset, out.data(), out.size())) {
        out.clear();
        return false;
    }
    DecryptRange(out.data(), out.size(), 0);
    return true;
}

bool Pf8Reader::ReadRange(const Pf8Entry &e, uint64_t offset, size_t len,
                          std::vector<uint8_t> &out) const {
    out.clear();
    if (e.offset == 0 || e.size == 0 || len == 0) return true;
    if (offset >= e.size) return false;
    const size_t avail = static_cast<size_t>(e.size - offset);
    const size_t want = len < avail ? len : avail;
    out.resize(want);
    if (!ReadAt(e.offset + offset, out.data(), want)) {
        out.clear();
        return false;
    }
    DecryptRange(out.data(), out.size(), offset);
    return true;
}

bool Pf8Reader::Read(const std::string &name, std::vector<uint8_t> &out) const {
    Pf8Entry e;
    if (!Find(name, e)) return false;
    return Read(e, out);
}

} // namespace artc
