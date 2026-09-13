// block_decode_regressions.cpp — DXT5/BC7 atlas decoding.

#include "render/block_decode.h"

#include <iostream>
#include <stdexcept>
#include <vector>

namespace {

int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) { ++g_failures; std::cerr << "FAIL: " << what << "\n"; }
}
void Put16(std::vector<uint8_t> &b, uint16_t v) { b.push_back(uint8_t(v)); b.push_back(uint8_t(v >> 8)); }

// A 4x4 solid red block: RGB565 0xF800 twice, all color indices 0, alpha 255.
std::vector<uint8_t> RedDxt5Block() {
    std::vector<uint8_t> b;
    b.push_back(255); b.push_back(0);      // a0 > a1, alpha index 0 -> 255
    for (int i = 0; i < 6; ++i) b.push_back(0);
    Put16(b, 0xF800); Put16(b, 0xF800);    // c0 == c1 (4-color mode not used)
    for (int i = 0; i < 4; ++i) b.push_back(0);
    return b;
}

void TestDxt5SolidRed() {
    const auto block = RedDxt5Block();
    std::vector<uint8_t> out;
    artc::DecodeDxt5Blocks(block, 4, 4, out);
    Check(out.size() == 4 * 4 * 4, "dxt5 output size");
    bool all_red = true;
    for (size_t i = 0; i < out.size(); i += 4)
        if (out[i] != 255 || out[i + 1] != 0 || out[i + 2] != 0 || out[i + 3] != 255)
            all_red = false;
    Check(all_red, "dxt5 solid red block decodes to opaque red");
}

void TestDxt5PartialSize() {
    // One 4x4 block, but the image is 4x3: the fourth row is not written.
    const auto block = RedDxt5Block();
    std::vector<uint8_t> out;
    artc::DecodeDxt5Blocks(block, 4, 3, out);
    Check(out.size() == 4 * 3 * 4, "dxt5 partial output size");
    Check(out[0] == 255 && out[3] == 255, "dxt5 partial first pixel");
}

void TestDxt5Truncated() {
    std::vector<uint8_t> out;
    bool threw = false;
    try { artc::DecodeDxt5Blocks(std::vector<uint8_t>(8, 0), 4, 4, out); }
    catch (const std::exception &) { threw = true; }
    Check(threw, "dxt5 truncated block throws");
}

void TestBc7Truncated() {
    std::vector<uint8_t> out;
    bool threw = false;
    try { artc::DecodeBc7Blocks(std::vector<uint8_t>(8, 0), 4, 4, out); }
    catch (const std::exception &) { threw = true; }
    Check(threw, "bc7 truncated block throws");
}

} // namespace

int main() {
    TestDxt5SolidRed();
    TestDxt5PartialSize();
    TestDxt5Truncated();
    TestBc7Truncated();
    if (g_failures == 0) std::cout << "block_decode_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
