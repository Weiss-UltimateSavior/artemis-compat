// byteutil.h — small shared byte helpers (hex key parsing, hexdump)
#pragma once
#include <cstdint>
#include <cctype>
#include <string>
#include <vector>

namespace artc {

// Parse "b3 28 3a ..." / "b3283a..." hex into bytes. Returns false on bad input.
inline bool ParseHexKey(const std::string &hex, std::vector<uint8_t> &out) {
    std::string clean;
    clean.reserve(hex.size());
    for (char c : hex) {
        if (c == ' ' || c == '\t' || c == ':' || c == '-') continue;
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        clean.push_back(c);
    }
    if (clean.empty() || clean.size() % 2) return false;
    out.clear();
    out.reserve(clean.size() / 2);
    auto nib = [](char c) -> int {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return (c >= '0' && c <= '9') ? c - '0' : c - 'a' + 10;
    };
    for (size_t i = 0; i < clean.size(); i += 2)
        out.push_back(static_cast<uint8_t>(nib(clean[i]) * 16 + nib(clean[i + 1])));
    return true;
}

inline std::string ToHex(const uint8_t *data, size_t len) {
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(len * 3);
    for (size_t i = 0; i < len; ++i) {
        if (i) s += ' ';
        s += d[data[i] >> 4];
        s += d[data[i] & 0xF];
    }
    return s;
}

// Stable map for path separators used inside pack names ('\\' on Windows packs).
inline std::string NormalizePackName(const std::string &name) {
    std::string s = name;
    for (char &c : s)
        if (c == '\\') c = '/';
    return s;
}

} // namespace artc
