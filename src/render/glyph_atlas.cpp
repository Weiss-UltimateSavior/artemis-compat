#include "render/glyph_atlas.h"
// Single TU instantiating the stb_truetype implementation: glyph_atlas is
// compiled for every target (the cache itself is GL-free and regression-
// tested without a context), so the rasterizer follows it here.
#define STB_TRUETYPE_IMPLEMENTATION
#include "render/stb_truetype.h"

#include <algorithm>
#include <cmath>

namespace artc {

void GlyphAtlas::Invalidate() {
    cache_.clear();
    pages_.clear();
}

void GlyphAtlas::NewPage() {
    pages_.emplace_back();
    pages_.back().pixels.assign(
        static_cast<size_t>(Page::kWidth) * Page::kHeight * 4, 0);
}

const GlyphAtlas::Entry *GlyphAtlas::Lookup(const stbtt_fontinfo *info,
                                             const Key &key) {
    const auto cached = cache_.find(key);
    if (cached != cache_.end()) {
        ++hits_;
        return &cached->second;
    }
    ++misses_;

    // ---- rasterize + composite (behavior-identical to the former per-SetText
    // cell build; each glyph occupies its own padded cell so overlapping
    // outlines and kerning never reveal neighbouring letters) ----
    int gw = 0, gh = 0, xoff = 0, yoff = 0;
    uint8_t *bmp = stbtt_GetGlyphBitmap(info, key.scale, key.scale, key.glyph,
                                        &gw, &gh, &xoff, &yoff);
    const int cw = gw + 2 * key.outline, ch = gh + 2 * key.outline;
    if (cw + 2 > Page::kWidth || ch + 2 > Page::kHeight) {
        stbtt_FreeBitmap(bmp, nullptr);
        return nullptr; // legacy degenerate-width failure
    }
    std::vector<uint8_t> cov(static_cast<size_t>(cw) * ch, 0),
        pixels(cov.size() * 4, 0);
    if (bmp)
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x)
                cov[static_cast<size_t>(y + key.outline) * cw + x + key.outline] =
                    bmp[y * gw + x];
    stbtt_FreeBitmap(bmp, nullptr);
    for (int y = 0; y < ch; ++y)
        for (int x = 0; x < cw; ++x) {
            const size_t off = static_cast<size_t>(y) * cw + x;
            const float fill = cov[off] / 255.f;
            uint8_t border = 0;
            if (key.outline)
                for (int oy = -key.outline; oy <= key.outline; ++oy)
                    for (int ox = -key.outline; ox <= key.outline; ++ox) {
                        const int xx = x + ox, yy = y + oy;
                        if (xx >= 0 && xx < cw && yy >= 0 && yy < ch &&
                            ox * ox + oy * oy <= key.outline * key.outline)
                            border = std::max(border,
                                              cov[static_cast<size_t>(yy) * cw + xx]);
                    }
            const float edge = border / 255.f * (1 - fill), alpha = fill + edge;
            if (alpha <= 0) continue;
            for (int c = 0; c < 3; ++c) {
                const int shift = (2 - c) * 8;
                pixels[off * 4 + c] = static_cast<uint8_t>(
                    (((key.color >> shift) & 255) * fill +
                     ((key.outline_color >> shift) & 255) * edge) /
                    alpha);
            }
            pixels[off * 4 + 3] = static_cast<uint8_t>(std::lround(alpha * 255));
        }

    // ---- pack into the last page (row wrap → next page → watermark eviction)
    if (pages_.empty()) NewPage();
    if (pages_.back().cursor_x + cw + 1 > Page::kWidth) { // wrap to a new row
        pages_.back().cursor_x = 1;
        pages_.back().cursor_y += pages_.back().row_h + 2;
        pages_.back().row_h = 0;
    }
    if (pages_.back().cursor_y + ch + 1 > Page::kHeight) { // page full
        if (pages_.size() >= kMaxPages) {
            Invalidate(); // watermark: drop everything, rebuild from here
            reset_ = true;
            NewPage();
        } else {
            NewPage();
        }
    }
    Page &page = pages_.back();
    const int x = page.cursor_x, y = page.cursor_y;
    for (int row = 0; row < ch; ++row)
        std::copy_n(pixels.data() + static_cast<size_t>(row) * cw * 4,
                    static_cast<size_t>(cw) * 4,
                    page.pixels.data() +
                        (static_cast<size_t>(y + row) * Page::kWidth + x) * 4);
    page.cursor_x = x + cw + 2;
    page.row_h = std::max(page.row_h, ch);
    ++page.generation;

    Entry e;
    e.page = pages_.size() - 1;
    e.u0 = float(x) / Page::kWidth;
    e.u1 = float(x + cw) / Page::kWidth;
    e.v0 = float(y) / Page::kHeight;
    e.v1 = float(y + ch) / Page::kHeight;
    e.w = cw;
    e.h = ch;
    e.xoff = xoff;
    e.yoff = yoff;
    return &cache_.emplace(key, e).first->second;
}

} // namespace artc
