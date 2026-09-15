// glyph_atlas.h — persistent glyph raster cache (GL-free).
//
// SetText's per-glyph rasterization (stbtt coverage → color/outline
// composite) is deterministic in (glyph index, pixel scale, outline width,
// fill color, outline color), so cells are rasterized once and packed into
// fixed-size atlas pages that survive SetText calls. Repeat text — typewriter
// re-sets, message page flips, repeated UI strings — then costs no
// stbtt rasterization, no CPU compositing and (see the compositor) no GPU
// upload at all.
//
// The GL side stays in the compositor: it uploads page pixels (a full-page
// glTexSubImage2D only when a page's generation advanced) and binds
// per-page textures at draw. Keeping this unit GL-free lets the cache be
// regression-tested without a context.
#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct stbtt_fontinfo;

namespace artc {

class GlyphAtlas {
public:
    struct Key {
        int glyph = 0;             // font glyph index (font changes invalidate)
        float scale = 0;           // stbtt pixel scale (main or ruby)
        int outline = 0;           // outline radius in pixels
        uint32_t color = 0;        // fill 0xRRGGBB
        uint32_t outline_color = 0;
        bool operator<(const Key &o) const {
            if (glyph != o.glyph) return glyph < o.glyph;
            if (scale != o.scale) return scale < o.scale;
            if (outline != o.outline) return outline < o.outline;
            if (color != o.color) return color < o.color;
            return outline_color < o.outline_color;
        }
    };
    struct Entry {
        size_t page = 0;           // index into Pages()
        float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
        int w = 0, h = 0;          // cell size incl. outline padding
        int xoff = 0, yoff = 0;    // stbtt glyph offset within the cell
    };
    struct Page {
        static constexpr int kWidth = 1024, kHeight = 1024;
        std::vector<uint8_t> pixels;   // kWidth*kHeight*4 RGBA, CPU copy
        uint64_t generation = 0;       // bumped on every placement
        int cursor_x = 1, cursor_y = 1, row_h = 0;
    };

    // Returns the cell for the key, rasterizing + packing on a miss.
    // nullptr only when the cell cannot fit a page at all (the legacy
    // degenerate-width failure). A watermark eviction may have rebuilt the
    // cache first — the caller must re-resolve when WasReset() reports it.
    const Entry *Lookup(const stbtt_fontinfo *info, const Key &key);

    const std::vector<Page> &Pages() const { return pages_; }
    // True when the last Lookup triggered the page-watermark eviction.
    // Entry copies held before it (layer glyphs, uploaded textures) are
    // stale: callers must drop them and re-resolve.
    bool WasReset() const { return reset_; }
    void ClearReset() { reset_ = false; }
    // Font change / GL loss: drop entries and pages completely.
    void Invalidate();

    size_t Hits() const { return hits_; }
    size_t Misses() const { return misses_; }

private:
    void NewPage();

    // ~170 cells per 1024^2 page at typical 64x80 glyph boxes; six pages
    // keep pathological style diversity bounded while normal dialogue never
    // comes close. Reaching the watermark evicts everything and rebuilds.
    static constexpr size_t kMaxPages = 6;

    std::map<Key, Entry> cache_;
    std::vector<Page> pages_;
    size_t hits_ = 0, misses_ = 0;
    bool reset_ = false;
};

} // namespace artc
