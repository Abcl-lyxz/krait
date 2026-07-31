#pragma once

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace krait::core::vt {

class Grid;
class ClusterPool;
struct Line;

// One row's worth of cells as UTF-8 text.
//
// Trailing blanks are dropped: a grid row is always `cols` wide, and searching
// a screenful of 80-column rows would otherwise mean searching mostly padding.
// A wide cluster's trailing half contributes nothing — it is the same character
// as its lead, not a second one.
std::string lineText(const Line& line, const ClusterPool& pool);

// Where a match landed. `line` counts scrollback first, then the viewport, so
// it is stable while nothing scrolls; `begin`/`end` are BYTE offsets into that
// line's text, which is what a UTF-8 highlight needs.
struct SearchHit {
    std::size_t line = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
};

struct SearchOptions {
    bool regex = false;
    bool caseSensitive = false;
    // Hostile-input bound. Scrollback holds hundreds of thousands of lines and
    // `.` matches all of them; a search that returns every one is a search that
    // hangs the UI thread rendering its own results.
    std::size_t maxHits = 5000;
};

// Searches scrollback, then the viewport.
//
// Returns an error rather than throwing for a bad pattern: a search box is user
// input, half-typed regexes are the normal case, and `[` must show "not a valid
// pattern" instead of taking the process with it. std::regex is the one thing
// in src/core that can throw, and it is caught here so nothing escapes
// (rules/cpp.md: no exceptions across module boundaries).
std::expected<std::vector<SearchHit>, std::string>
searchScrollback(const Grid& grid, std::string_view pattern, const SearchOptions& options);

// Smart selection: what a double-click should take.
//
// Byte offsets into `text`, half-open. The rules run in order of how SPECIFIC
// they are — a URL beats a path beats a word — because the more specific answer
// is the one someone double-clicking a URL wanted, and taking only the hostname
// out of one is the classic terminal annoyance.
struct SmartSelection {
    std::size_t begin = 0;
    std::size_t end = 0;
};

SmartSelection smartSelect(std::string_view text, std::size_t at);

}  // namespace krait::core::vt
