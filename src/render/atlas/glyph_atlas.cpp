#include "render/atlas/glyph_atlas.h"

#include <algorithm>
#include <utility>

namespace krait::render {

std::size_t GlyphAtlas::KeyHash::operator()(const GlyphKey& key) const {
    return (static_cast<std::size_t>(key.faceId) << 32) ^ key.glyphId;
}

GlyphAtlas::GlyphAtlas(int cellWidth, int lineHeight)
    // A slot holds a 2-cell glyph, plus padding on each side so a bilinear tap
    // at a slot edge cannot pick up the neighbour's coverage.
    : m_slotWidth(std::max(1, (2 * cellWidth) + (2 * kPadding))),
      m_slotHeight(std::max(1, lineHeight + (2 * kPadding))),
      m_pixels(static_cast<std::size_t>(kWidth) * kInitialHeight, 0) {}

const AtlasEntry* GlyphAtlas::peek(GlyphKey key) const {
    const auto it = m_index.find(key);
    return it == m_index.end() ? nullptr : &it->second->entry;
}

const AtlasEntry* GlyphAtlas::get(GlyphKey key, const RasterFn& raster) {
    if (const auto it = m_index.find(key); it != m_index.end()) {
        m_lru.splice(m_lru.begin(), m_lru, it->second);
        return &it->second->entry;
    }

    GlyphBitmap bitmap;
    if (!raster || !raster(key.faceId, key.glyphId, bitmap)) {
        return nullptr;
    }
    // A glyph wider or taller than a slot cannot be cached. Rejecting it is the
    // honest answer: silently clipping would draw a wrong glyph, which is worse
    // than drawing none. Space glyphs legitimately have a zero-size bitmap and
    // still need an entry, so only the upper bound is a rejection.
    if (bitmap.width + (2 * kPadding) > m_slotWidth ||
        bitmap.height + (2 * kPadding) > m_slotHeight) {
        return nullptr;
    }

    std::uint32_t slotIndex = 0;
    if (!m_freeSlots.empty()) {
        slotIndex = m_freeSlots.back();
        m_freeSlots.pop_back();
    } else {
        if (m_nextSlot >= capacity() && !makeRoom()) {
            return nullptr;
        }
        if (!m_freeSlots.empty()) {  // makeRoom evicted rather than grew
            slotIndex = m_freeSlots.back();
            m_freeSlots.pop_back();
        } else {
            slotIndex = m_nextSlot++;
        }
    }

    Slot slot;
    slot.index = slotIndex;
    slot.key = key;
    blit(bitmap, slotIndex, slot.entry);

    m_lru.push_front(std::move(slot));
    m_index[key] = m_lru.begin();
    return &m_lru.front().entry;
}

bool GlyphAtlas::makeRoom() {
    // Grow first — a bigger atlas keeps everything resident, and growth is
    // free of the churn eviction causes. Height only, so no placed glyph moves.
    if (m_height < kMaxHeight) {
        const int grown = std::min(kMaxHeight, m_height * 2);
        m_pixels.resize(static_cast<std::size_t>(kWidth) * grown, 0);
        m_height = grown;
        m_grew = true;
        return true;
    }

    // At the cap: evict the least recently used glyph and reuse its slot.
    if (m_lru.empty()) {
        return false;
    }
    const Slot& oldest = m_lru.back();
    m_index.erase(oldest.key);
    m_freeSlots.push_back(oldest.index);
    m_lru.pop_back();
    ++m_evictions;
    return true;
}

void GlyphAtlas::blit(const GlyphBitmap& bitmap, std::uint32_t slotIndex, AtlasEntry& entry) {
    const std::size_t perRow = slotsPerRow();
    const auto slotX = static_cast<int>(slotIndex % perRow) * m_slotWidth;
    const auto slotY = static_cast<int>(slotIndex / perRow) * m_slotHeight;
    const int x = slotX + kPadding;
    const int y = slotY + kPadding;

    // The slot may hold an evicted glyph's pixels; clear before writing so the
    // padding gutter and any smaller footprint do not show the previous glyph.
    for (int row = 0; row < m_slotHeight; ++row) {
        const auto begin =
            (static_cast<std::size_t>(slotY + row) * kWidth) + static_cast<std::size_t>(slotX);
        std::fill_n(m_pixels.begin() + static_cast<std::ptrdiff_t>(begin), m_slotWidth, 0);
    }
    for (int row = 0; row < bitmap.height; ++row) {
        const auto dst = (static_cast<std::size_t>(y + row) * kWidth) + static_cast<std::size_t>(x);
        const auto src = static_cast<std::size_t>(row) * static_cast<std::size_t>(bitmap.width);
        std::copy_n(bitmap.gray.begin() + static_cast<std::ptrdiff_t>(src), bitmap.width,
                    m_pixels.begin() + static_cast<std::ptrdiff_t>(dst));
    }

    entry.x = static_cast<std::uint16_t>(x);
    entry.y = static_cast<std::uint16_t>(y);
    entry.width = static_cast<std::uint16_t>(bitmap.width);
    entry.height = static_cast<std::uint16_t>(bitmap.height);
    entry.bearingX = static_cast<std::int16_t>(bitmap.bearingX);
    entry.bearingY = static_cast<std::int16_t>(bitmap.bearingY);

    if (!dirty()) {
        m_dirtyTop = slotY;
        m_dirtyBottom = slotY + m_slotHeight;
    } else {
        m_dirtyTop = std::min(m_dirtyTop, slotY);
        m_dirtyBottom = std::max(m_dirtyBottom, slotY + m_slotHeight);
    }
}

void GlyphAtlas::clearDirty() {
    m_dirtyTop = 0;
    m_dirtyBottom = 0;
}

bool GlyphAtlas::takeGrew() {
    return std::exchange(m_grew, false);
}

void GlyphAtlas::clear() {
    m_lru.clear();
    m_index.clear();
    m_freeSlots.clear();
    m_nextSlot = 0;
    std::ranges::fill(m_pixels, std::uint8_t{0});
    m_dirtyTop = 0;
    m_dirtyBottom = m_height;
}

}  // namespace krait::render
