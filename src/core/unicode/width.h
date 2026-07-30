#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace krait::core::unicode {

// Grapheme clustering + display width (T19). This is the ONLY sanctioned way to
// ask "how many cells does this take" — a bare per-codepoint wcwidth-style call
// is banned by rules/vt-core.md and rejected on sight in review, because width
// is a property of a CLUSTER, not of a codepoint.
//
// Backed by utf8proc (ADR-0003), which ships UCD 17.0.0 tables; we generate
// none of our own. Evidence and the measured traps:
// docs/research/t19-width-findings.md.

// East-Asian-Ambiguous resolution. Codepoints in EA width class A (Greek,
// Cyrillic, box drawing, ...) are 1 cell in a western font and 2 in many CJK
// fonts. There is no right answer, only a per-session setting — and mode 2027
// is how an application negotiates it, never a guess on our side.
enum class Ambiguous : std::uint8_t { Narrow, Wide };

// One extended grapheme cluster (UAX#29) inside the codepoint span it came
// from: `begin` and `end` are indices, `end` exclusive.
struct Cluster {
    std::size_t begin = 0;
    std::size_t end = 0;
    int width = 0;  // display cells: 0, 1 or 2
};

// Walks a codepoint span, yielding one Cluster at a time. Stateful by
// necessity: UAX#29 GB10/12/13 (emoji ZWJ sequences and regional-indicator
// pairing) cannot be decided from a codepoint pair alone, so breaks MUST be
// evaluated in order over the whole span. Constructing a fresh iterator per
// span is the only supported use — resuming across a resegmented buffer is not.
class ClusterIterator {
  public:
    ClusterIterator(std::span<const char32_t> text, Ambiguous ambiguous) noexcept;

    // Returns false when the span is exhausted.
    bool next(Cluster& out) noexcept;

  private:
    std::span<const char32_t> m_text;
    Ambiguous m_ambiguous;
    std::size_t m_pos = 0;
    std::int32_t m_breakState = 0;  // utf8proc's; 0 means "start of text"
};

// The streaming counterpart of ClusterIterator, for the one caller that cannot
// use it: the grid. A terminal receives codepoints one at a time from the
// parser and must decide, on each, "does this extend the cluster already in the
// current cell, or start a new one?" — there is no span to iterate over.
//
// Same engine, same ordering requirement: every consecutive pair is fed to
// utf8proc in order, including the pair that breaks, because that call is what
// arms GB12/13 for the next cluster. Feed codepoints out of order and regional
// indicators (flags) and ZWJ sequences silently mis-segment.
class ClusterBreaker {
  public:
    // True when `cp` STARTS a new cluster, false when it extends the previous
    // one. The first codepoint after construction or reset() always starts one.
    bool startsNewCluster(char32_t cp) noexcept;

    // Forget the previous codepoint. Callers must do this whenever the
    // codepoint stream is discontinuous — a cluster cannot span a cursor jump,
    // an erase, or a screen swap.
    void reset() noexcept;

  private:
    char32_t m_prev = 0;
    bool m_hasPrev = false;
    std::int32_t m_breakState = 0;
};

// Width of a single already-segmented cluster. Exposed because the grid needs
// it for a cluster it is re-measuring (a reflow, or a cell it already stores)
// without re-running segmentation.
//
// The rules, in the order they apply:
//   1. base width = the first codepoint's width, NOT a sum — a cluster's
//      trailing combining marks are width 0 and must not add cells.
//   2. VS16 (U+FE0F) anywhere in the cluster forces 2; VS15 (U+FE0E) forces 1.
//      Both variation selectors are themselves width 0, so this cannot be
//      expressed additively.
//   3. A regional-indicator pair (a flag) is 2, even though each half is 1.
//   4. If the base is EA class A and still 1 cell, the Ambiguous setting
//      decides. Applied LAST, and only to printable width-1 codepoints —
//      utf8proc reports combining marks and soft hyphen as ambiguous too, and
//      promoting those to 2 cells would be a rendering bug.
int clusterWidth(std::span<const char32_t> cluster, Ambiguous ambiguous) noexcept;

// The Unicode version backing the tables above, e.g. "17.0.0". Reported rather
// than assumed so a baseline bump that changes segmentation is visible.
const char* unicodeVersion() noexcept;

}  // namespace krait::core::unicode
