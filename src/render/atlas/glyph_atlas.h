#pragma once

#include <cstdint>
#include <functional>
#include <list>
#include <unordered_map>
#include <vector>

namespace krait::render {

// One rasterised glyph, as FreeType hands it over.
struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int bearingX = 0;                // px from the pen origin to the bitmap's left edge
    int bearingY = 0;                // px from the baseline UP to the bitmap's top edge
    std::vector<std::uint8_t> gray;  // width * height, 8-bit coverage
};

// Rasterising is a callback rather than an interface: there is exactly one
// production implementation (the shaper's FT_Face), and rules/cpp.md rejects an
// abstraction with a single implementation. A callback also lets the eviction
// tests run without a font on disk.
using RasterFn = std::function<bool(std::uint32_t faceId, std::uint32_t glyphId, GlyphBitmap& out)>;

// Where a glyph lives in the atlas texture.
struct AtlasEntry {
    std::uint16_t x = 0;
    std::uint16_t y = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::int16_t bearingX = 0;
    std::int16_t bearingY = 0;
};

struct GlyphKey {
    std::uint32_t faceId = 0;
    std::uint32_t glyphId = 0;

    friend bool operator==(const GlyphKey&, const GlyphKey&) = default;
};

// The R8 glyph atlas (T25), with LRU eviction and a hard growth cap as
// rules/render.md requires.
//
// Uniform slots, not shelf packing. A general text engine packs glyphs of wildly
// different sizes and needs shelves; a TERMINAL's glyphs are all bounded by the
// cell, so a fixed slot grid is both simpler and strictly better here: placement
// is O(1), and — the part shelves get wrong — a freed slot is immediately
// reusable, which is what makes real LRU eviction possible at all. Shelf
// allocators cannot free, so they end up clearing the whole atlas under
// pressure, which is a frame spike rather than an eviction policy.
//
// ponytail: a slot is wide enough for a 2-cell glyph, so narrow glyphs waste
// about half their slot. At 2048x512 R8 that is 1 MB of texture, which is not
// worth a second size class. Add one only if a measurement says otherwise.
//
// The texture GROWS IN HEIGHT ONLY, with the width fixed. That is deliberate:
// the slot pitch never changes, so growing cannot move an already-placed glyph
// and no atlas coordinate handed out earlier goes stale.
class GlyphAtlas {
  public:
    static constexpr int kWidth = 2048;
    static constexpr int kInitialHeight = 512;
    static constexpr int kMaxHeight = 4096;  // the growth cap
    static constexpr int kPadding = 1;       // keeps bilinear taps off the neighbour

    // Slot size comes from the font's cell metrics. `cellWidth` is one cell; a
    // slot holds two so a wide cluster's glyph fits.
    GlyphAtlas(int cellWidth, int lineHeight);

    // The entry for a glyph, rasterising on a miss. nullptr only if the glyph
    // cannot be rasterised or is too large for a slot — a caller that gets
    // nullptr skips drawing that glyph rather than failing the frame.
    const AtlasEntry* get(GlyphKey key, const RasterFn& raster);

    // Already-resident lookup that does NOT rasterise and does NOT count as a
    // use. For asserting residency in tests without perturbing the LRU order.
    const AtlasEntry* peek(GlyphKey key) const;

    int width() const { return kWidth; }

    int height() const { return m_height; }

    const std::vector<std::uint8_t>& pixels() const { return m_pixels; }

    // The region written since the last clearDirty(), as a row range. Rows
    // rather than a rect because uploads go by scanline and a tight rect would
    // not pay for itself: a frame touches a handful of new glyphs at most.
    bool dirty() const { return m_dirtyTop < m_dirtyBottom; }

    int dirtyTop() const { return m_dirtyTop; }

    int dirtyBottom() const { return m_dirtyBottom; }  // exclusive

    void clearDirty();

    // True when the texture grew since the last call, so the renderer knows it
    // must recreate the GPU texture rather than upload into the old one.
    bool takeGrew();

    std::size_t residentGlyphs() const { return m_lru.size(); }

    std::size_t capacity() const { return slotsPerRow() * slotRows(m_height); }

    std::uint64_t evictions() const { return m_evictions; }

    void clear();

  private:
    struct Slot {
        std::uint32_t index = 0;  // slot ordinal; position is derived from it
        AtlasEntry entry;
        GlyphKey key;
    };

    struct KeyHash {
        std::size_t operator()(const GlyphKey& key) const;
    };

    std::size_t slotsPerRow() const { return static_cast<std::size_t>(kWidth / m_slotWidth); }

    std::size_t slotRows(int height) const {
        return static_cast<std::size_t>(height / m_slotHeight);
    }

    // Grows the texture, or evicts the least recently used slot. False when
    // neither is possible.
    bool makeRoom();

    void blit(const GlyphBitmap& bitmap, std::uint32_t slotIndex, AtlasEntry& entry);

    int m_slotWidth = 1;
    int m_slotHeight = 1;
    int m_height = kInitialHeight;
    std::vector<std::uint8_t> m_pixels;

    // Front is most recently used.
    std::list<Slot> m_lru;
    std::unordered_map<GlyphKey, std::list<Slot>::iterator, KeyHash> m_index;
    std::vector<std::uint32_t> m_freeSlots;  // reclaimed by eviction
    std::uint32_t m_nextSlot = 0;            // never-yet-used slots
    std::uint64_t m_evictions = 0;

    int m_dirtyTop = 0;
    int m_dirtyBottom = 0;
    bool m_grew = false;
};

}  // namespace krait::render
