#pragma once

#include "core/graphics/image.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace krait::core::vt {

// Where a decoded image sits on the grid (M5 T79/T80).
//
// **Anchored to a STABLE LINE INDEX, never to a screen row.** This is the same
// landmine CLAUDE.md records for scrollback and OSC 133 marks, and it bites
// harder here: a row number is invalidated by every scroll, every eviction and
// every reflow, so an image stored beside the grid would slide onto different
// text the moment the user dragged the window. Scrollback::linesEverStarted()
// is monotone precisely so something can hold a reference across all three.
struct Placement {
    // The image this shows. Several placements may share one image — that is
    // the point of kitty's `a=p`, which places an already-transmitted image
    // again without resending the pixels.
    std::uint32_t imageId = 0;
    // The logical line the top-left corner sits on, in Scrollback's stable
    // space. Resolved through indexOfStable() at draw time.
    std::uint64_t anchor = 0;
    int col = 0;
    // Size in CELLS, not pixels: the grid speaks cells, and a placement sized
    // in pixels would need re-deriving on every font change.
    int cols = 0;
    int rows = 0;
    // Source rectangle in the image, in pixels. Zero width or height means the
    // whole image — the common case, and cheaper than storing the size twice.
    int srcX = 0;
    int srcY = 0;
    int srcW = 0;
    int srcH = 0;
    // Negative draws UNDER the text, which is what a background watermark is.
    int zIndex = 0;

    friend bool operator==(const Placement&, const Placement&) = default;
};

// Images and their placements for one terminal.
//
// Bounded in BYTES rather than in count, because that is the resource a remote
// host actually spends: a thousand 1x1 images are harmless and one 4000x4000 is
// not. Eviction is oldest-first by insertion, which for a terminal is also
// least-recently-useful — an image scrolled off the top is the one nobody will
// place again.
class ImageStore {
  public:
    // 64 MiB of decoded pixels per terminal. Generous enough for a screenshot
    // and a few plots, small enough that twenty tabs cannot exhaust a machine.
    static constexpr std::size_t kMaxBytes = 64u * 1024 * 1024;
    // A placement is a few dozen bytes; the cap exists so a host cannot spend
    // unbounded memory placing one 1x1 image a million times.
    static constexpr std::size_t kMaxPlacements = 4096;

    // Stores `image` under `id`, replacing anything already there. An id of 0
    // means "assign one", which is what sixel needs — it has no concept of an
    // image id at all. Returns the id used, or 0 when the image was refused.
    std::uint32_t put(std::uint32_t id, Image image);

    // Null when there is no such image.
    const Image* find(std::uint32_t id) const;

    // Adds a placement. Refused (false) when the image is unknown, so a
    // placement can never outlive the pixels it names.
    bool place(const Placement& placement);

    // Drops one image and every placement of it.
    void erase(std::uint32_t id);

    // Drops everything — what a full reset and an alternate-screen switch do.
    void clear();

    const std::vector<Placement>& placements() const noexcept { return m_placements; }

    std::size_t byteSize() const noexcept { return m_bytes; }

    std::size_t imageCount() const noexcept { return m_images.size(); }

    // Drops placements whose anchor line no longer exists. Called after
    // eviction: without it a long session accumulates placements pointing at
    // history that is gone, and the cap starts refusing new ones.
    void dropAnchorsBefore(std::uint64_t oldestStable);

  private:
    void evictUntilFits(std::size_t incoming);

    struct Entry {
        Image image;
        std::uint64_t sequence = 0;  // insertion order, for eviction
    };

    std::map<std::uint32_t, Entry> m_images;
    std::vector<Placement> m_placements;
    std::size_t m_bytes = 0;
    std::uint64_t m_sequence = 0;
    std::uint32_t m_nextAutoId = 1;
};

}  // namespace krait::core::vt
