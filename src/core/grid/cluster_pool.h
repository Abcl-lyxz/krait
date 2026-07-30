#pragma once

#include "core/grid/cell.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

namespace krait::core::vt {

// Side table for grapheme clusters that do not fit in a Cell's single char32_t
// (base + combining marks, ZWJ emoji sequences, Thai syllables). Cells store a
// tagged index into this pool; see cell.h for the encoding.
//
// Interning is not an optimisation here, it is the bound. A screen of Thai or
// emoji re-prints the same handful of clusters constantly, and without dedupe a
// scrolling document would exhaust the cap in seconds.
//
// ponytail: entries are never freed, so the ceiling is "kMaxClusters DISTINCT
// clusters per session, ever" — after that, new clusters degrade to their base
// codepoint and lose their marks. That is bounded and graceful, which is what
// rules/net.md asks of hostile input; it is not permanent. Upgrade path if a
// real long-lived session ever exhausts it: refcount entries against live cells
// and reuse a free list. Do not reach for that before a measurement says so.
class ClusterPool {
  public:
    static constexpr std::uint32_t kMaxClusters = 65'536;

    // Returns the char32_t to store in a Cell. Single-codepoint clusters are
    // returned as-is and never occupy a slot, so the common case allocates
    // nothing at all.
    char32_t intern(std::span<const char32_t> cluster);

    // The codepoints behind a stored `ch`. A literal codepoint yields an empty
    // span — callers hold their own single-codepoint buffer for that case.
    std::span<const char32_t> lookup(char32_t ch) const;

    std::size_t size() const { return m_clusters.size(); }

  private:
    // deque, NOT vector: m_index's keys are views INTO these strings, and a
    // vector reallocation moves every short-string-optimised buffer and
    // dangles the entire index at once. deque never moves an existing element.
    std::deque<std::u32string> m_clusters;
    std::unordered_map<std::u32string_view, std::uint32_t> m_index;
};

}  // namespace krait::core::vt
