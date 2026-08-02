#include "core/grid/images.h"

#include <algorithm>
#include <utility>

namespace krait::core::vt {

void ImageStore::evictUntilFits(std::size_t incoming) {
    // Oldest first. For a terminal that is also least-recently-useful: an image
    // that scrolled off the top is the one nobody is going to place again.
    while (!m_images.empty() && m_bytes + incoming > kMaxBytes) {
        const auto oldest = std::ranges::min_element(
            m_images, {}, [](const auto& entry) { return entry.second.sequence; });
        erase(oldest->first);
    }
}

std::uint32_t ImageStore::put(std::uint32_t id, Image image) {
    if (image.empty()) {
        return 0;
    }
    const std::size_t bytes = image.byteSize();
    // One image larger than the whole budget is refused outright rather than
    // evicting everything to make room for something that still will not fit.
    if (bytes > kMaxBytes) {
        return 0;
    }

    if (id == 0) {
        // Sixel has no image ids at all, so it gets one. Wrapping at the top
        // rather than saturating: the counter is only ever compared for
        // equality, and a saturated one would make every later sixel overwrite
        // the same slot.
        id = m_nextAutoId++;
        if (m_nextAutoId == 0) {
            m_nextAutoId = 1;
        }
    }

    erase(id);  // replacing an id drops the old pixels AND its placements
    evictUntilFits(bytes);
    if (m_bytes + bytes > kMaxBytes) {
        return 0;  // still no room, and everything evictable is gone
    }

    m_bytes += bytes;
    m_images.emplace(id, Entry{.image = std::move(image), .sequence = m_sequence++});
    return id;
}

const Image* ImageStore::find(std::uint32_t id) const {
    const auto it = m_images.find(id);
    return it == m_images.end() ? nullptr : &it->second.image;
}

bool ImageStore::place(const Placement& placement) {
    // A placement naming an image that is not here is refused rather than
    // stored hopefully: pixels never arrive later, and a dangling placement is
    // a blank rectangle nobody can account for.
    if (m_images.find(placement.imageId) == m_images.end()) {
        return false;
    }
    if (placement.cols <= 0 || placement.rows <= 0) {
        return false;
    }
    if (m_placements.size() >= kMaxPlacements) {
        // Drop the OLDEST placement, not the new one. A host that placed four
        // thousand images is scrolling a plot; the newest is the one on screen.
        m_placements.erase(m_placements.begin());
    }
    m_placements.push_back(placement);
    return true;
}

void ImageStore::erase(std::uint32_t id) {
    const auto it = m_images.find(id);
    if (it == m_images.end()) {
        return;
    }
    m_bytes -= it->second.image.byteSize();
    m_images.erase(it);
    std::erase_if(m_placements,
                  [id](const Placement& placement) { return placement.imageId == id; });
}

void ImageStore::clear() {
    m_images.clear();
    m_placements.clear();
    m_bytes = 0;
    // m_sequence and m_nextAutoId deliberately keep counting. Resetting the id
    // counter would let a new sixel land on an id a still-running application
    // believes it owns.
}

void ImageStore::dropAnchorsBefore(std::uint64_t oldestStable) {
    std::erase_if(m_placements, [oldestStable](const Placement& placement) {
        return placement.anchor < oldestStable;
    });
}

}  // namespace krait::core::vt
