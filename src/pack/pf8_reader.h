// pf8_reader.h — reader for the pf8 pack format.
//
// Format spec (behavior-level, cross-verified against the official packer's
// published algorithm and multiple game samples):
//
//   offset 0x00  char magic[2]     = "pf"
//   offset 0x02  uint8  version    = '8'
//   offset 0x03  u32   index_size  (byte length of the hashed index region,
//                                  counted from offset 7)
//   offset 0x07  u32   file_count
//   offset 0x0B  records, stride = 16 + name_len:
//                  u32 name_len
//                  char name[name_len]   (pack-internal separator '\')
//                  u32 pad               (observed 0)
//                  u32 data_offset       (absolute; 0 = empty entry)
//                  u32 data_size
//
// Key derivation (version '8'):
//   key = SHA1( file[7 : 7 + index_size] )        (20 bytes)
// File data decryption: byte-wise XOR, key phase restarts at each file's
// data start. Entries with offset == 0 are empty placeholders.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace artc {

struct Pf8Entry {
    std::string name;   // raw name as stored (use NormalizePackName for lookup)
    uint64_t offset;
    uint32_t size;
};

class Pf8Reader {
public:
    // `key` empty (default) → derive automatically via SHA1 over the index
    // region. An explicit key overrides derivation.
    bool Open(const std::string &path, const std::vector<uint8_t> &key = {});

    const std::vector<Pf8Entry> &Entries() const { return entries_; }
    uint32_t FileCount() const { return static_cast<uint32_t>(entries_.size()); }
    const std::string &Path() const { return path_; }
    const std::vector<uint8_t> &Key() const { return key_; }

    // Case-insensitive-ish lookup: exact, then normalized-separator match.
    bool Find(const std::string &name, Pf8Entry &out) const;

    // Read+decrypt one entry into `out`. Returns false on IO/lookup failure.
    bool Read(const Pf8Entry &e, std::vector<uint8_t> &out) const;
    bool Read(const std::string &name, std::vector<uint8_t> &out) const;

private:
    void Decrypt(uint8_t *data, size_t len) const;

    std::string path_;
    std::vector<uint8_t> key_;
    std::vector<Pf8Entry> entries_;
};

} // namespace artc
