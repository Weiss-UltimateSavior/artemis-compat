// sha1.h — compact public-domain-style SHA-1 (FIPS 180-1) for the pf8 key
// derivation: key = SHA1(header_index_region). Portable, no external deps.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace artc {

inline uint32_t Sha1Rol(uint32_t v, int b) { return (v << b) | (v >> (32 - b)); }

inline void Sha1Block(uint32_t state[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
    for (int i = 16; i < 80; ++i)
        w[i] = Sha1Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        const uint32_t tmp = Sha1Rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Sha1Rol(b, 30); b = a; a = tmp;
    }
    state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
}

inline void Sha1(const uint8_t *data, size_t len, uint8_t out[20]) {
    uint32_t state[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t full = (len / 64) * 64;
    for (size_t i = 0; i < full; i += 64) Sha1Block(state, data + i);

    uint8_t tail[128];
    const size_t rem = len - full;
    std::memcpy(tail, data + full, rem);
    tail[rem] = 0x80;
    const size_t tail_len = (rem + 9 <= 64) ? 64 : 128;
    std::memset(tail + rem + 1, 0, tail_len - rem - 1);
    const uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 0; i < 8; ++i) tail[tail_len - 1 - i] = uint8_t(bits >> (8 * i));
    Sha1Block(state, tail);
    if (tail_len == 128) Sha1Block(state, tail + 64);

    for (int i = 0; i < 5; ++i) {
        out[i * 4]     = uint8_t(state[i] >> 24);
        out[i * 4 + 1] = uint8_t(state[i] >> 16);
        out[i * 4 + 2] = uint8_t(state[i] >> 8);
        out[i * 4 + 3] = uint8_t(state[i]);
    }
}

inline std::string Sha1Hex(const uint8_t *data, size_t len) {
    uint8_t d[20];
    Sha1(data, len, d);
    char buf[41];
    for (int i = 0; i < 20; ++i) std::snprintf(buf + i * 2, 3, "%02x", d[i]);
    return std::string(buf, 40);
}

} // namespace artc
