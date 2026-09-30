#include "render/block_decode.h"
#include "render/bc7decomp.h"

#include <stdexcept>

namespace artc {

void DecodeDxt5Blocks(const std::vector<uint8_t> &src, int width, int height,
                      std::vector<uint8_t> &out) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const size_t base = (static_cast<size_t>(by) * bw + bx) * 16;
            if (base + 16 > src.size()) throw std::runtime_error("truncated DXT5 block");
            const uint8_t a0 = src[base], a1 = src[base + 1];
            uint8_t alpha[8];
            alpha[0] = a0;
            alpha[1] = a1;
            if (a0 > a1) {
                for (int i = 0; i < 6; ++i) alpha[2 + i] = static_cast<uint8_t>(((6 - i) * a0 + (1 + i) * a1) / 7);
            } else {
                for (int i = 0; i < 4; ++i) alpha[2 + i] = static_cast<uint8_t>(((4 - i) * a0 + (1 + i) * a1) / 5);
                alpha[6] = 0;
                alpha[7] = 255;
            }
            uint64_t a_bits = 0;
            for (int i = 0; i < 6; ++i) a_bits |= uint64_t(src[base + 2 + i]) << (8 * i);
            const uint16_t c0 = static_cast<uint16_t>(src[base + 8] | (src[base + 9] << 8));
            const uint16_t c1 = static_cast<uint16_t>(src[base + 10] | (src[base + 11] << 8));
            auto rgb565 = [](uint16_t c, uint8_t *p) {
                p[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
                p[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
                p[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
            };
            uint8_t pal[4][3];
            rgb565(c0, pal[0]);
            rgb565(c1, pal[1]);
            if (c0 > c1) {
                for (int i = 0; i < 3; ++i) {
                    pal[2][i] = static_cast<uint8_t>((2 * pal[0][i] + pal[1][i]) / 3);
                    pal[3][i] = static_cast<uint8_t>((pal[0][i] + 2 * pal[1][i]) / 3);
                }
            } else {
                for (int i = 0; i < 3; ++i) pal[2][i] = static_cast<uint8_t>((pal[0][i] + pal[1][i]) / 2);
                pal[3][0] = pal[3][1] = pal[3][2] = 0;
            }
            uint32_t c_bits = 0;
            for (int i = 0; i < 4; ++i) c_bits |= uint32_t(src[base + 12 + i]) << (8 * i);
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int px = bx * 4 + x, py = by * 4 + y;
                    if (px >= width || py >= height) continue;
                    const int idx = y * 4 + x;
                    uint8_t *dst = out.data() + (static_cast<size_t>(py) * width + px) * 4;
                    const int ci = (c_bits >> (2 * idx)) & 3;
                    const int ai = (a_bits >> (3 * idx)) & 7;
                    dst[0] = pal[ci][0];
                    dst[1] = pal[ci][1];
                    dst[2] = pal[ci][2];
                    dst[3] = alpha[ai];
                }
        }
}

namespace {
// Shared BC1 colour expansion: 565 endpoints plus the 2-bit index table.
// `punch` selects the c0<=c1 3-colour + transparent mode (DXT1 only).
void Bc1Colors(const uint8_t *block, bool punch, uint8_t colors[4][4]) {
    const uint16_t c0 = uint16_t(block[0] | (block[1] << 8));
    const uint16_t c1 = uint16_t(block[2] | (block[3] << 8));
    const auto rgb = [](uint16_t c, uint8_t *o) {
        o[0] = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
        o[1] = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
        o[2] = uint8_t((c & 0x1F) * 255 / 31);
    };
    rgb(c0, colors[0]);
    rgb(c1, colors[1]);
    colors[0][3] = colors[1][3] = 255;
    if (!punch || c0 > c1) {
        for (int k = 0; k < 3; ++k) {
            colors[2][k] = uint8_t((2 * colors[0][k] + colors[1][k]) / 3);
            colors[3][k] = uint8_t((colors[0][k] + 2 * colors[1][k]) / 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (int k = 0; k < 3; ++k) {
            colors[2][k] = uint8_t((colors[0][k] + colors[1][k]) / 2);
            colors[3][k] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;  // punch-through black
    }
}
} // namespace

void DecodeDxt1Blocks(const std::vector<uint8_t> &src, int width, int height,
                      std::vector<uint8_t> &out) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (src.size() < size_t(bw) * bh * 8) throw std::runtime_error("truncated DXT1 block");
    out.assign(size_t(width) * height * 4, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t *block = src.data() + (size_t(by) * bw + bx) * 8;
            uint8_t colors[4][4];
            Bc1Colors(block, true, colors);
            const uint32_t bits = uint32_t(block[4]) | (uint32_t(block[5]) << 8) |
                                  (uint32_t(block[6]) << 16) | (uint32_t(block[7]) << 24);
            for (int py = 0; py < 4; ++py)
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    const int index = (bits >> (2 * (py * 4 + px))) & 3;
                    std::copy_n(colors[index], 4, out.data() + (size_t(y) * width + x) * 4);
                }
        }
}

void DecodeDxt3Blocks(const std::vector<uint8_t> &src, int width, int height,
                      std::vector<uint8_t> &out) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (src.size() < size_t(bw) * bh * 16) throw std::runtime_error("truncated DXT3 block");
    out.assign(size_t(width) * height * 4, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            const uint8_t *block = src.data() + (size_t(by) * bw + bx) * 16;
            uint64_t alpha = 0;
            for (int i = 0; i < 8; ++i) alpha |= uint64_t(block[i]) << (8 * i);
            uint8_t colors[4][4];
            Bc1Colors(block + 8, false, colors);
            const uint32_t bits = uint32_t(block[12]) | (uint32_t(block[13]) << 8) |
                                  (uint32_t(block[14]) << 16) | (uint32_t(block[15]) << 24);
            for (int py = 0; py < 4; ++py)
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    const int index = (bits >> (2 * (py * 4 + px))) & 3;
                    uint8_t *o = out.data() + (size_t(y) * width + x) * 4;
                    std::copy_n(colors[index], 3, o);
                    o[3] = uint8_t(((alpha >> (4 * (py * 4 + px))) & 0xF) * 17);
                }
        }
}

void DecodeBc7Blocks(const std::vector<uint8_t> &src, int width, int height,
                     std::vector<uint8_t> &out) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    if (src.size() < static_cast<size_t>(bw) * bh * 16) throw std::runtime_error("truncated BC7 block");
    out.assign(static_cast<size_t>(width) * height * 4, 0);
    for (int by = 0; by < bh; ++by)
        for (int bx = 0; bx < bw; ++bx) {
            bc7decomp::color_rgba px[16];
            if (!bc7decomp::unpack_bc7(src.data() + (static_cast<size_t>(by) * bw + bx) * 16, px))
                throw std::runtime_error("invalid BC7 block");
            for (int y = 0; y < 4; ++y)
                for (int x = 0; x < 4; ++x) {
                    const int dx = bx * 4 + x, dy = by * 4 + y;
                    if (dx >= width || dy >= height) continue;
                    uint8_t *dst = out.data() + (static_cast<size_t>(dy) * width + dx) * 4;
                    dst[0] = px[y * 4 + x].r;
                    dst[1] = px[y * 4 + x].g;
                    dst[2] = px[y * 4 + x].b;
                    dst[3] = px[y * 4 + x].a;
                }
        }
}

} // namespace artc
