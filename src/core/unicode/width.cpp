#include "core/unicode/width.h"

#include <utf8proc.h>

namespace krait::core::unicode {

namespace {

constexpr char32_t kVs15 = 0xFE0E;  // text presentation
constexpr char32_t kVs16 = 0xFE0F;  // emoji presentation
constexpr char32_t kRegionalFirst = 0x1F1E6;
constexpr char32_t kRegionalLast = 0x1F1FF;

bool isRegionalIndicator(char32_t cp) noexcept {
    return cp >= kRegionalFirst && cp <= kRegionalLast;
}

// utf8proc takes a signed codepoint and its docs warn that anything outside
// 0..0x10FFFF may crash it. Our UTF-8 decoder already rejects those, but width
// is also reachable from stored cells and from reflow, so re-check here rather
// than trust a caller three layers away with hostile bytes behind it.
bool isScalarValue(char32_t cp) noexcept {
    return cp <= 0x10FFFF && !(cp >= 0xD800 && cp <= 0xDFFF);
}

int codepointWidth(char32_t cp) noexcept {
    if (!isScalarValue(cp)) {
        return 0;
    }
    return utf8proc_charwidth(static_cast<utf8proc_int32_t>(cp));
}

}  // namespace

int clusterWidth(std::span<const char32_t> cluster, Ambiguous ambiguous) noexcept {
    if (cluster.empty()) {
        return 0;
    }
    const char32_t base = cluster.front();

    // Rule 2 first, because a variation selector overrides everything the base
    // would otherwise say — and it is width 0, so it can never be summed in.
    for (const char32_t cp : cluster) {
        if (cp == kVs16) {
            return 2;
        }
        if (cp == kVs15) {
            return 1;
        }
    }

    // Rule 3: a flag. Each regional indicator is width 1, so the base-width
    // rule alone would render a flag half-width. Two is the pair; a lone
    // trailing indicator (an odd-length run) stays 1 and falls through.
    if (isRegionalIndicator(base) && cluster.size() >= 2 && isRegionalIndicator(cluster[1])) {
        return 2;
    }

    // Rule 1: the BASE's width, never a sum. Trailing marks are width 0 by
    // table, but summing would still be wrong the moment a cluster contains a
    // second spacing codepoint.
    int width = codepointWidth(base);

    // Rule 4, last and narrowly gated. utf8proc reports combining marks, both
    // variation selectors, and soft hyphen as EA class A; promoting any of
    // those to two cells would be a rendering bug, so require the base to
    // already be a printable single cell. Cf/Mn/Me/Cc/Cs are excluded
    // explicitly — width 1 alone does not prove printable (soft hyphen is Cf
    // and width 1).
    if (width == 1 && ambiguous == Ambiguous::Wide && isScalarValue(base) &&
        utf8proc_charwidth_ambiguous(static_cast<utf8proc_int32_t>(base))) {
        switch (utf8proc_category(static_cast<utf8proc_int32_t>(base))) {
        case UTF8PROC_CATEGORY_MN:
        case UTF8PROC_CATEGORY_ME:
        case UTF8PROC_CATEGORY_CF:
        case UTF8PROC_CATEGORY_CC:
        case UTF8PROC_CATEGORY_CS:
            break;
        default:
            width = 2;
            break;
        }
    }
    return width;
}

ClusterIterator::ClusterIterator(std::span<const char32_t> text, Ambiguous ambiguous) noexcept
    : m_text(text), m_ambiguous(ambiguous) {}

bool ClusterIterator::next(Cluster& out) noexcept {
    if (m_pos >= m_text.size()) {
        return false;
    }
    const std::size_t begin = m_pos;
    std::size_t end = begin + 1;
    // Extend while utf8proc says there is NO break between the pair. The state
    // must see every consecutive pair in order, including the pair that breaks
    // — that call is what arms GB12/13 for the next cluster, so it happens here
    // and its result decides the loop rather than being recomputed later.
    while (end < m_text.size()) {
        const char32_t prev = m_text[end - 1];
        const char32_t cur = m_text[end];
        if (!isScalarValue(prev) || !isScalarValue(cur)) {
            break;  // never hand utf8proc a non-scalar; treat it as a boundary
        }
        if (utf8proc_grapheme_break_stateful(static_cast<utf8proc_int32_t>(prev),
                                             static_cast<utf8proc_int32_t>(cur), &m_breakState)) {
            break;
        }
        ++end;
    }
    out.begin = begin;
    out.end = end;
    out.width = clusterWidth(m_text.subspan(begin, end - begin), m_ambiguous);
    m_pos = end;
    return true;
}

const char* unicodeVersion() noexcept {
    return utf8proc_unicode_version();
}

}  // namespace krait::core::unicode
