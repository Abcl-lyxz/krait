// Grapheme clustering + width (T19). Expectations were MEASURED against the
// utf8proc our baseline resolves, not recalled — see
// docs/research/t19-width-findings.md for the probe output and the traps.
#include "core/unicode/width.h"
#include <catch2/catch_test_macros.hpp>

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

using krait::core::unicode::Ambiguous;
using krait::core::unicode::Cluster;
using krait::core::unicode::ClusterIterator;
using krait::core::unicode::clusterWidth;

namespace {

using Shape = std::vector<std::pair<std::size_t, int>>;

// Segments a whole span, so a test asserts the SHAPE of the segmentation and
// not just one boundary. Each entry is {codepoint count, display width}.
Shape segment(const std::vector<char32_t>& text, Ambiguous amb = Ambiguous::Narrow) {
    Shape out;
    ClusterIterator it(text, amb);
    Cluster c;
    while (it.next(c)) {
        out.emplace_back(c.end - c.begin, c.width);
    }
    return out;
}

int widthOf(const std::vector<char32_t>& cluster, Ambiguous amb = Ambiguous::Narrow) {
    return clusterWidth(cluster, amb);
}

}  // namespace

TEST_CASE("width: tables come from the Unicode version we think", "[width]") {
    // A baseline bump that moves segmentation should be visible, not silent.
    // docs/research/t19-width-findings.md records 17.0.0.
    CHECK(std::string(krait::core::unicode::unicodeVersion()) == "17.0.0");
}

// Keep TEST_CASE names ASCII: catch_discover_tests round-trips them through a
// ctest filter, and a non-ASCII character is mangled by the console codepage
// until the test can no longer be matched by its own name. It reports as a
// failure that has nothing to do with the assertions.
TEST_CASE("width: T19 acceptance case - farmer emoji is one cluster of 2", "[width][emoji]") {
    // U+1F9D1 ZWJ U+1F33E. Both breaks join, so this is ONE cluster, and its
    // width comes from the base (2) rather than a sum over parts (which would
    // give 4 and tear the grid).
    CHECK(segment({0x1F9D1, 0x200D, 0x1F33E}) == Shape{{3, 2}});
}

TEST_CASE("width: cluster width is the base's, never a sum", "[width]") {
    CHECK(widthOf({U'e', 0x301}) == 1);  // e + combining acute
    CHECK(widthOf({0x1F9D1, 0x200D, 0x1F33E}) == 2);
    CHECK(widthOf({U'A'}) == 1);
    CHECK(widthOf({0x6F22}) == 2);  // 漢
    CHECK(widthOf({0xAC00}) == 2);  // 가, precomposed Hangul syllable
    CHECK(widthOf({}) == 0);
}

TEST_CASE("width: variation selectors override the base, both ways", "[width][emoji]") {
    // U+26A0 is width 1 by table (text presentation default). VS16 must promote
    // it to 2 even though VS16 is itself width 0 — so this cannot be additive.
    CHECK(widthOf({0x26A0}) == 1);
    CHECK(widthOf({0x26A0, 0xFE0F}) == 2);
    // ...and VS15 must demote an emoji-presentation base back to 1.
    CHECK(widthOf({0x1F9D1}) == 2);
    CHECK(widthOf({0x1F9D1, 0xFE0E}) == 1);
}

TEST_CASE("width: a flag is 2 cells, not 1", "[width][emoji]") {
    // Each regional indicator is width 1, so "width of the base" alone would
    // render every flag half-width. This is the one case the base rule misses.
    CHECK(widthOf({0x1F1E6}) == 1);           // lone indicator
    CHECK(widthOf({0x1F1E6, 0x1F1E9}) == 2);  // 🇦🇩
    // Two indicators segment as ONE cluster (UAX#29 GB12/13), and a third
    // starts a new one — the pairing is stateful, which is why the iterator
    // must see every break in order.
    CHECK(segment({0x1F1E6, 0x1F1E9, 0x1F1E6, 0x1F1E9}) == Shape{{2, 2}, {2, 2}});
    CHECK(segment({0x1F1E6, 0x1F1E9, 0x1F1E6}) == Shape{{2, 2}, {1, 1}});
}

TEST_CASE("width: Thai clusters break at spacing vowels", "[width][thai]") {
    // ก + mai-ek + sara-a. The mai-ek (U+0E48, Mn, width 0) joins its base, but
    // sara-a (U+0E30) is a SPACING vowel in category Lo, so UAX#29 correctly
    // breaks before it: two clusters of one cell each, NOT one cluster.
    CHECK(segment({0x0E01, 0x0E48, 0x0E30}) == Shape{{2, 1}, {1, 1}});
    // สวัสดี — the above-base marks attach to their consonants, so this is four
    // clusters and four cells, not six. Getting it wrong is how Thai mangles.
    CHECK(segment({0x0E2A, 0x0E27, 0x0E31, 0x0E2A, 0x0E14, 0x0E35}) ==
          Shape{{1, 1}, {2, 1}, {1, 1}, {2, 1}});
}

TEST_CASE("width: East-Asian-Ambiguous follows the setting, not a guess", "[width][eaa]") {
    // Genuine EA class A: 1 in a western font, 2 in many CJK fonts.
    CHECK(widthOf({0x03B1}, Ambiguous::Narrow) == 1);  // α
    CHECK(widthOf({0x03B1}, Ambiguous::Wide) == 2);
    CHECK(widthOf({0x2500}, Ambiguous::Narrow) == 1);  // ─ box drawing
    CHECK(widthOf({0x2500}, Ambiguous::Wide) == 2);
    // Unambiguous codepoints must not move.
    CHECK(widthOf({U'A'}, Ambiguous::Wide) == 1);
    CHECK(widthOf({0x6F22}, Ambiguous::Wide) == 2);
}

TEST_CASE("width: the ambiguous setting never promotes a non-printable", "[width][eaa]") {
    // utf8proc reports combining marks, BOTH variation selectors, and soft
    // hyphen as EA class A. Promoting any of those to two cells would be a
    // rendering bug, so the setting is gated on printability — this is the trap
    // a naive `if (ambiguous) width = 2` walks straight into.
    CHECK(widthOf({0x0301}, Ambiguous::Wide) == 0);       // combining acute, Mn
    CHECK(widthOf({0x00AD}, Ambiguous::Wide) == 1);       // soft hyphen, Cf
    CHECK(widthOf({U'e', 0x301}, Ambiguous::Wide) == 1);  // base decides, not the mark
}

TEST_CASE("width: hostile codepoints are a boundary, never a crash", "[width]") {
    // utf8proc's docs warn that a codepoint outside 0..0x10FFFF "might crash"
    // it. Width is reachable from stored cells and from reflow, not just from
    // the decoder, so it re-checks rather than trusting its caller.
    CHECK(widthOf({0x110000}) == 0);
    CHECK(widthOf({0xD800}) == 0);  // unpaired surrogate
    // A non-scalar mid-span ends the cluster instead of reaching utf8proc.
    CHECK(segment({U'a', 0xD800, U'b'}) == Shape{{1, 1}, {1, 0}, {1, 1}});
}

TEST_CASE("width: iterator is exhaustive and covers every codepoint once", "[width]") {
    const std::vector<char32_t> text{U'a', 0x301, 0x6F22, 0x1F9D1, 0x200D, 0x1F33E, U'z'};
    ClusterIterator it(text, Ambiguous::Narrow);
    Cluster c;
    std::size_t expectedBegin = 0;
    int clusters = 0;
    while (it.next(c)) {
        CHECK(c.begin == expectedBegin);  // no gaps, no overlap
        CHECK(c.end > c.begin);           // always makes progress
        expectedBegin = c.end;
        ++clusters;
    }
    CHECK(expectedBegin == text.size());
    CHECK(clusters == 4);
    CHECK_FALSE(it.next(c));  // exhausted stays exhausted
}

TEST_CASE("width: empty input yields no clusters", "[width]") {
    ClusterIterator it({}, Ambiguous::Narrow);
    Cluster c;
    CHECK_FALSE(it.next(c));
}
