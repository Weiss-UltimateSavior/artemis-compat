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
