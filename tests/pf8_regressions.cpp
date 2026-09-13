// pf8_regressions.cpp — synthesized PFS container regressions.
//
// Builds tiny in-memory packs (encrypted pf8 and clear-text pf2), then checks
// entry parsing, case-insensitive lookup, full reads and ranged reads. No game
// assets are involved; all payloads are generated here.

#include "pack/pf8_reader.h"
#include "pack/sha1.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

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

struct FileSpec {
    std::string name;
    std::vector<uint8_t> data;
};

// Serialize a pf8/pf2 container. Encrypted packs XOR the payload with
// SHA1(file_count || records) exactly as the reader derives it.
std::vector<uint8_t> BuildPack(char version, const std::vector<FileSpec> &files) {
    // Record table (records only; file_count lives at offset 7).
    std::vector<uint8_t> records;
    for (const auto &f : files) {
        Put32(records, static_cast<uint32_t>(f.name.size()));
        records.insert(records.end(), f.name.begin(), f.name.end());
        Put32(records, 0); // pad
        Put32(records, 0); // data offset placeholder
        Put32(records, static_cast<uint32_t>(f.data.size()));
    }
    const uint32_t index_size = static_cast<uint32_t>(records.size() + 4);
    const uint32_t data_start = 7 + index_size;

    // Lay out the payload and fix the record offsets first; the derived key is
    // SHA1 over the *final* record bytes, so it must be computed afterwards.
    std::vector<uint8_t> data;
    size_t record_pos = 0;
    for (const auto &f : files) {
        const uint32_t off = data_start + static_cast<uint32_t>(data.size());
        const size_t field = record_pos + 4 + f.name.size() + 4;
        records[field + 0] = off & 0xFF;
        records[field + 1] = (off >> 8) & 0xFF;
        records[field + 2] = (off >> 16) & 0xFF;
        records[field + 3] = (off >> 24) & 0xFF;
        data.insert(data.end(), f.data.begin(), f.data.end());
        record_pos += 4 + f.name.size() + 12;
    }

    std::vector<uint8_t> hashed;
    Put32(hashed, static_cast<uint32_t>(files.size()));
    hashed.insert(hashed.end(), records.begin(), records.end());

    std::vector<uint8_t> key;
    if (version == '8') {
        key.resize(20);
        artc::Sha1(hashed.data(), hashed.size(), key.data());
        size_t offset = 0;
        for (const auto &f : files) {
            for (size_t i = 0; i < f.data.size(); ++i)
                data[offset + i] ^= key[i % key.size()];
            offset += f.data.size();
        }
    }

    std::vector<uint8_t> out;
    out.push_back('p');
    out.push_back('f');
    out.push_back(static_cast<uint8_t>(version));
    Put32(out, index_size);
    // The hashed region starts at offset 7: file_count then records.
    Put32(out, static_cast<uint32_t>(files.size()));
    out.insert(out.end(), records.begin(), records.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

std::string WriteTemp(const std::vector<uint8_t> &bytes, const std::string &name) {
    const auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    out.close();
    return path.string();
}

void TestEncryptedCaseAndRange() {
    std::vector<FileSpec> files = {
        {"Data\\Background.PNG", {'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H'}},
        {"data/background.png", {'1', '2', '3', '4'}}, // duplicate key, later
        {"script/init.lua", {'x'}},
    };
    const std::string path = WriteTemp(BuildPack('8', files), "artc_pf8_test.pfs");
    artc::Pf8Reader reader;
    Check(reader.Open(path), "open encrypted pack");
    Check(reader.Encrypted(), "pf8 reports encrypted");
    Check(reader.FileCount() == 3, "three entries");

    artc::Pf8Entry e;
    Check(reader.Find("data/background.png", e), "case-insensitive find");
    // First occurrence wins (the upper-case name was inserted first).
    Check(e.name == "Data\\Background.PNG", "first match wins");

    std::vector<uint8_t> full;
    Check(reader.Read(e, full), "full read");
    Check(full == std::vector<uint8_t>({'A','B','C','D','E','F','G','H'}), "decrypted payload");

    // Ranged reads must equal slices of the full read.
    for (size_t off = 0; off < full.size(); ++off) {
        for (size_t len = 1; off + len <= full.size(); ++len) {
            std::vector<uint8_t> part;
            Check(reader.ReadRange(e, off, len, part), "range read");
            Check(part == std::vector<uint8_t>(full.begin() + off, full.begin() + off + len),
                  "range equals slice");
        }
    }
    std::filesystem::remove(path);
}

void TestClearTextPf2() {
    std::vector<FileSpec> files = {
        {"movie/logo.mp4", {'M', 'O', 'V', 'I', 'E'}},
    };
    const std::string path = WriteTemp(BuildPack('2', files), "artc_pf2_test.pfs");
    artc::Pf8Reader reader;
    Check(reader.Open(path), "open pf2 pack");
    Check(!reader.Encrypted(), "pf2 reports clear text");
    std::vector<uint8_t> got;
    Check(reader.Read("MOVIE/logo.mp4", got), "pf2 case-insensitive read");
    Check(got == std::vector<uint8_t>({'M','O','V','I','E'}), "pf2 payload in the clear");
    std::filesystem::remove(path);
}

} // namespace

int main() {
    TestEncryptedCaseAndRange();
    TestClearTextPf2();
    if (g_failures == 0) std::cout << "pf8_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
