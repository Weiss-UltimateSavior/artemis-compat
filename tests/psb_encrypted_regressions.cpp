// psb_encrypted_regressions.cpp — encrypted-header PSB decode.
//
// The reference layout (FreeMote/NekoMiko v4, PSB v3 here) derives the header
// keystream seed from the canonical header length, then XORs the header body
// with a shifting 32-bit cipher. This test encrypts an otherwise plain PSB
// header and checks it decodes back to the same tree.

#include "pack/psb.h"
#include "emote_scene_fixture.h"

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

artc::PsbDocument SampleDocument() {
    artc::PsbDocument doc;
    doc.root.type = artc::PsbValue::Object;
    artc::PsbValue n; n.type = artc::PsbValue::Number; n.number = 42;
    doc.root.object["answer"] = n;
    artc::PsbValue s; s.type = artc::PsbValue::String; s.string = "hello";
    doc.root.object["text"] = s;
    return doc;
}

void TestEncryptedHeaderRoundTrip() {
    const artc::PsbDocument doc = SampleDocument();
    std::vector<uint8_t> plain = emote_fixture::EncodePsb(doc);
    Check(plain.size() >= 44, "v3 header present");
    Put32(plain, 8, 44);       // header_length (the inference anchor)
    plain[6] = 1;              // encryption_flags
    plain[7] = 0;
    Cipher(0x0BADF00Du).Apply(plain.data() + 8, 44 - 8);

    artc::PsbDocument decoded;
    std::string error;
    Check(artc::DecodePsb(plain, decoded, error), "decode encrypted header: " + error);
    Check(decoded.root.At("answer").Num(-1) == 42, "encrypted tree preserved (number)");
    Check(decoded.root.At("text").string == "hello", "encrypted tree preserved (string)");
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
    TestPlainStillDecodes();
    if (g_failures == 0) std::cout << "psb_encrypted_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
