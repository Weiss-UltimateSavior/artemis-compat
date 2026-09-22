// pf8_reader.h — reader for the pf8 pack format.
//
// Format spec (behavior-level, cross-verified against the official packer's
// published algorithm and multiple game samples):
//
//   offset 0x00  char magic[2]     = "pf"
//   offset 0x02  uint8  version    = '8' (encrypted) | '2'/'6' (PF6 layout,
//                                    unencrypted)
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
// data start. Entries with offset == 0 are empty placeholders. Version '2'/'6'
// keeps the same record layout but stores data in the clear (no key).
//
// Entry lookup is case-insensitive (ASCII fold) and the first matching record
// wins, mirroring the original engine's historical behavior.
#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace artc {

struct Pf8Entry {
    std::string name;   // raw name as stored (use NormalizePackName for lookup)
    uint64_t offset;
    uint32_t size;
};

class Pf8Reader {
public:
    ~Pf8Reader();
    // Holds an open file descriptor for the pack; copies would double-close.
    Pf8Reader() = default;
    Pf8Reader(const Pf8Reader &) = delete;
    Pf8Reader &operator=(const Pf8Reader &) = delete;

    // `key` empty (default) → derive automatically via SHA1 over the index
    // region. An explicit key overrides derivation.
    bool Open(const std::string &path, const std::vector<uint8_t> &key = {});
    // Open an already-authorized descriptor. The reader owns and closes fd.
    bool OpenFd(const std::string &path, int fd,
                const std::vector<uint8_t> &key = {});

    const std::vector<Pf8Entry> &Entries() const { return entries_; }
    uint32_t FileCount() const { return static_cast<uint32_t>(entries_.size()); }
    const std::string &Path() const { return path_; }
    const std::vector<uint8_t> &Key() const { return key_; }
    // false for the unencrypted PF6-era '2'/'6' containers (no derived key).
    bool Encrypted() const { return encrypted_; }

    // Case-insensitive-ish lookup: exact, then normalized-separator match.
    // O(1) through the lookup index built at Open (first record wins, so
    // duplicate normalized names keep the earliest entry).
    bool Find(const std::string &name, Pf8Entry &out) const;

    // Read+decrypt one entry into `out`. Returns false on IO/lookup failure.
    bool Read(const Pf8Entry &e, std::vector<uint8_t> &out) const;
    bool Read(const std::string &name, std::vector<uint8_t> &out) const;

    // Read `len` bytes starting at `offset` inside the entry, decrypting with
    // the key phase that applies at that position. Used for streamed reads
    // (movie/audio) without loading the whole entry.
    bool ReadRange(const Pf8Entry &e, uint64_t offset, size_t len,
                   std::vector<uint8_t> &out) const;

private:
    // Positional read off the retained pack handle (pread on POSIX — safe
    // for concurrent reads; the legacy fopen path on Windows).
    bool ReadAt(uint64_t offset, void *buf, size_t len) const;
    void DecryptRange(uint8_t *data, size_t len, uint64_t offset) const;
    void Close();

    std::string path_;
    std::vector<uint8_t> key_;
    bool encrypted_ = true;
    std::vector<Pf8Entry> entries_;
    // normalized entry name → index of the FIRST matching record (the
    // original engine's first-record-wins lookup), built once at Open.
    std::unordered_map<std::string, size_t> index_;
#if !defined(_WIN32)
    int fd_ = -1;
#endif
};

} // namespace artc
