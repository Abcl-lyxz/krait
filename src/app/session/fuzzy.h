#pragma once

#include <string_view>

namespace krait::app::session {

// Subsequence fuzzy match, the kind a command palette needs: every character of
// `query` must appear in `text` in order, and the score rewards the matches a
// human would call obvious — a prefix, a word start, a contiguous run.
//
// Case-insensitive over ASCII only. Thai and CJK have no case to fold, so
// folding them is a no-op rather than a bug; what matters is that a
// non-matching byte never advances the query cursor.
//
// Returns -1 for no match. Any other value is comparable ONLY against scores
// from the same query: the scale is relative, not absolute.
int fuzzyScore(std::string_view query, std::string_view text);

// True when `query` is empty (everything matches) or scores at all. Kept
// separate so a caller filtering a list does not have to know that -1 is the
// sentinel.
inline bool fuzzyMatches(std::string_view query, std::string_view text) {
    return query.empty() || fuzzyScore(query, text) >= 0;
}

}  // namespace krait::app::session
