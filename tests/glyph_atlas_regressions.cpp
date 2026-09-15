// glyph_atlas_regressions.cpp — persistent glyph raster cache behavior.
//
// Uses the synthetic rectangle-glyph font fixture (tests/data/rectangle.ttf,
// see generate_font.py): 'A' is a solid 600×700 box, so coverage and pixel
// placement are exactly predictable. No commercial font data.

#include "render/glyph_atlas.h"
#include "stb_truetype.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#ifndef ARTC_TEST_DATA
#define ARTC_TEST_DATA "data"
#endif

namespace {
int g_failures = 0;
void Check(bool ok, const std::string &what) {
    if (!ok) {
        ++g_failures;
        std::cerr << "FAIL: " << what << "\n";
    }
}
} // namespace

int main() {
    std::ifstream file(std::string(ARTC_TEST_DATA) + "/rectangle.ttf",
                       std::ios::binary);
    Check(bool(file), "open synthetic font");
    if (!file) return 1;
    std::vector<unsigned char> font{std::istreambuf_iterator<char>(file), {}};

    stbtt_fontinfo info;
    const int offset = stbtt_GetFontOffsetForIndex(font.data(), 0);
    Check(offset >= 0, "font offset");
    if (offset < 0 || !stbtt_InitFont(&info, font.data(), offset)) {
        std::cerr << "FAIL: stbtt_InitFont\n";
        return 1;
    }
    const int glyph_a = stbtt_FindGlyphIndex(&info, 'A');
    Check(glyph_a > 0, "glyph index for 'A'");

    // Key.scale is the stbtt font-unit → pixel scale (what SetText gets from
    // stbtt_ScaleForMappingEmToPixels), not the point size.
    const float s32 = stbtt_ScaleForMappingEmToPixels(&info, 32.0f);
    const float s48 = stbtt_ScaleForMappingEmToPixels(&info, 48.0f);

    artc::GlyphAtlas atlas;

    // ---- miss then hit: the same key resolves the same entry, no re-raster ----
    const size_t m0 = atlas.Misses(), h0 = atlas.Hits();
    const artc::GlyphAtlas::Entry *e1 =
        atlas.Lookup(&info, {glyph_a, s32, 0, 0xffffff, 0});
    Check(e1 != nullptr, "first lookup places a cell");
    if (!e1) return 1;
    Check(atlas.Misses() == m0 + 1, "first lookup is a miss");
    const artc::GlyphAtlas::Entry *e1b =
        atlas.Lookup(&info, {glyph_a, s32, 0, 0xffffff, 0});
    Check(e1b == e1, "repeat lookup returns the same entry");
    Check(atlas.Hits() == h0 + 1, "repeat lookup is a hit");
    Check(atlas.Misses() == m0 + 1, "repeat lookup adds no miss");
    Check(e1->w > 0 && e1->h > 0, "'A' cell has extent");

    // ---- the cell actually contains coverage: solid rectangle → an opaque
    // pixel near the cell interior ----
    {
        const auto &page = atlas.Pages()[e1->page];
        const int px = static_cast<int>(e1->u0 * artc::GlyphAtlas::Page::kWidth);
        const int py = static_cast<int>(e1->v0 * artc::GlyphAtlas::Page::kHeight);
        size_t opaque = 0;
        for (int y = 0; y < e1->h; ++y)
            for (int x = 0; x < e1->w; ++x) {
                const size_t off =
                    (static_cast<size_t>(py + y) * artc::GlyphAtlas::Page::kWidth +
                     px + x) * 4;
                if (page.pixels[off + 3] > 250) ++opaque;
            }
        Check(opaque > 0, "cell pixels carry coverage");
    }

    // ---- style variations are distinct cache keys ----
    const artc::GlyphAtlas::Entry *outline =
        atlas.Lookup(&info, {glyph_a, s32, 2, 0xffffff, 0x000000});
    Check(outline != nullptr && outline != e1, "outline variant is a new cell");
    Check(outline->w == e1->w + 4 && outline->h == e1->h + 4,
          "outline padding widens the cell");
    const artc::GlyphAtlas::Entry *bigger =
        atlas.Lookup(&info, {glyph_a, s48, 0, 0xffffff, 0});
    Check(bigger != e1 && bigger->w > e1->w, "scale variant is a new, larger cell");
    const artc::GlyphAtlas::Entry *recolor =
        atlas.Lookup(&info, {glyph_a, s32, 0, 0xff0000, 0});
    Check(recolor != e1, "color variant is a new cell");

    // ---- cells on the same page do not overlap ----
    auto disjoint = [](const artc::GlyphAtlas::Entry *a,
                      const artc::GlyphAtlas::Entry *b) {
        return a->u1 <= b->u0 || b->u1 <= a->u0 || a->v1 <= b->v0 ||
               b->v1 <= a->v0;
    };
    Check(disjoint(e1, outline) && disjoint(e1, recolor) && disjoint(outline, recolor),
          "packed cells are pairwise disjoint");

    // ---- page generation advances on placement only ----
    const uint64_t gen = atlas.Pages()[e1->page].generation;
    atlas.Lookup(&info, {glyph_a, s32, 0, 0xffffff, 0}); // hit
    Check(atlas.Pages()[e1->page].generation == gen, "hit does not touch the page");
    atlas.Lookup(&info, {glyph_a, s32, 1, 0xffffff, 0}); // new placement
    Check(atlas.Pages()[e1->page].generation == gen + 1,
          "placement bumps the page generation");

    // ---- invalidate drops everything; the next lookup re-rasterizes ----
    atlas.Invalidate();
    Check(atlas.Pages().empty(), "invalidate clears pages");
    Check(atlas.Lookup(&info, {glyph_a, s32, 0, 0xffffff, 0}) != nullptr,
          "lookup after invalidate re-places");
    Check(atlas.Misses() == m0 + 6, "post-invalidate lookup is a miss");
    Check(!atlas.WasReset(), "manual invalidate is not an eviction");

    if (g_failures == 0) std::cout << "glyph_atlas_regressions: ok\n";
    return g_failures == 0 ? 0 : 1;
}
