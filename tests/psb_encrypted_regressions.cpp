// psb_encrypted_regressions.cpp — encrypted-header PSB decode.
//
// The reference layout (FreeMote/NekoMiko v4, PSB v3 here) derives the header
// keystream seed from the canonical header length, then XORs the header body
// with a shifting 32-bit cipher. This test encrypts an otherwise plain PSB
// header and checks it decodes back to the same tree.

#include "pack/psb.h"
#include "emote_scene_fixture.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}

constexpr uint32_t kKey1 = 123456789u;
constexpr uint32_t kKey2 = 362436069u;
constexpr uint32_t kKey3 = 521288629u;

struct Cipher {
    uint32_t key1 = kKey1, key2 = kKey2, key3 = kKey3, key4, current = 0;
    explicit Cipher(uint32_t seed) : key4(seed) {}
    void Apply(uint8_t *data, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            if (current == 0) {
                const uint32_t a = key1 ^ (key1 << 11), b = key4;
                const uint32_t next = a ^ b ^ ((a ^ (b >> 11)) >> 8);
                key1 = key2; key2 = key3; key3 = b; key4 = next; current = next;
            }
            data[i] ^= static_cast<uint8_t>(current);
            current >>= 8;
        }
    }
};

void Put32(std::vector<uint8_t> &b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t Adler32(const uint8_t *p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// Sign the v3 header payload ([8,40)) into the checksum word at offset 40.
void SignV3Header(std::vector<uint8_t> &plain) { Put32(plain, 40, Adler32(plain.data() + 8, 32)); }

artc::PsbDocument SampleDocument() {
    artc::PsbDocument doc;
    doc.root.type = artc::PsbValue::Object;
    artc::PsbValue n; n.type = artc::PsbValue::Number; n.number = 42;
    doc.root.object["answer"] = n;
    artc::PsbValue s; s.type = artc::PsbValue::String; s.string = "hello";
    doc.root.object["text"] = s;
    return doc;
}

std::vector<uint8_t> EncryptedFixture(uint32_t seed) {
    const artc::PsbDocument doc = SampleDocument();
    std::vector<uint8_t> plain = emote_fixture::EncodePsb(doc);
    Put32(plain, 8, 44);       // header_length (anchor derived by the decoder)
    SignV3Header(plain);       // adler32 of [8,40) -> offset 40
    plain[6] = 1;              // encryption_flags
    plain[7] = 0;
    Cipher(seed).Apply(plain.data() + 8, 44 - 8);
    return plain;
}

void TestEncryptedHeaderRoundTrip() {
    std::vector<uint8_t> bytes = EncryptedFixture(0x0BADF00Du);
    artc::PsbDocument decoded;
    std::string error;
    Check(artc::DecodePsb(bytes, decoded, error), "decode encrypted header: " + error);
    Check(decoded.root.At("answer").Num(-1) == 42, "encrypted tree preserved (number)");
    Check(decoded.root.At("text").string == "hello", "encrypted tree preserved (string)");
}

void TestExplicitSeed() {
    const std::vector<uint8_t> bytes = EncryptedFixture(0x1234ABCDe);
    ::setenv("ARTC_EMOTE_SEED", "0x1234abcd", 1);
    artc::PsbDocument decoded;
    std::string error;
    const bool ok = artc::DecodePsb(bytes, decoded, error);
    ::unsetenv("ARTC_EMOTE_SEED");
    Check(ok, "explicit seed decodes: " + error);
    Check(decoded.root.At("answer").Num(-1) == 42, "explicit seed tree preserved");
}

void TestChecksumRejectsCorruption() {
    std::vector<uint8_t> bytes = EncryptedFixture(0x0BADF00Du);
    bytes[20] ^= 0x01; // corrupt an encrypted header word
    artc::PsbDocument decoded;
    std::string error;
    Check(!artc::DecodePsb(bytes, decoded, error), "corrupt header rejected");
}

void TestPlainStillDecodes() {
    const artc::PsbDocument doc = SampleDocument();
    std::vector<uint8_t> plain = emote_fixture::EncodePsb(doc);
    artc::PsbDocument decoded;
    std::string error;
    Check(artc::DecodePsb(plain, decoded, error), "plain header still decodes");
    Check(decoded.root.At("answer").Num(-1) == 42, "plain tree preserved");
}

} // namespace

int main() {
    TestEncryptedHeaderRoundTrip();
    TestExplicitSeed();
    TestChecksumRejectsCorruption();
    TestPlainStillDecodes();
    if (g_failures == 0) std::cout << "psb_encrypted_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
