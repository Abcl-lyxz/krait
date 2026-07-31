#include "fuzzy.h"

namespace krait::app::session {

namespace {

constexpr int kMatch = 8;
constexpr int kContiguousBonus = 10;
constexpr int kWordStartBonus = 12;
constexpr int kFirstCharBonus = 16;
// Per skipped character, capped: a query that matches at position 3 and one
// that matches at position 300 should not rank the same, but a long path
// should not be pushed below a genuinely worse match either.
constexpr int kGapPenalty = 1;
constexpr int kMaxGapPenalty = 40;

char fold(char ch) {
    return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool isWordBoundary(char ch) {
    return ch == ' ' || ch == '-' || ch == '_' || ch == '/' || ch == '.' || ch == '@' || ch == ':';
}

}  // namespace

int fuzzyScore(std::string_view query, std::string_view text) {
    if (query.empty()) {
        return 0;
    }
    if (text.empty()) {
        return -1;
    }

    int score = 0;
    int gap = 0;
    std::size_t queryIndex = 0;
    bool lastWasMatch = false;

    // Greedy left-to-right. Not optimal — "ab" against "a-xa-b" takes the first
    // 'a' rather than the one at a word start — but it is O(n) and a palette is
    // judged on feeling instant across thousands of entries.
    // ponytail: greedy scan; swap for Smith-Waterman only if ranking complaints
    // show up in practice.
    for (std::size_t i = 0; i < text.size() && queryIndex < query.size(); ++i) {
        if (fold(text[i]) != fold(query[queryIndex])) {
            lastWasMatch = false;
            ++gap;
            continue;
        }
        score += kMatch;
        if (i == 0) {
            score += kFirstCharBonus;
        } else if (isWordBoundary(text[i - 1])) {
            score += kWordStartBonus;
        }
        if (lastWasMatch) {
            score += kContiguousBonus;
        }
        lastWasMatch = true;
        ++queryIndex;
    }

    if (queryIndex < query.size()) {
        return -1;  // ran out of text with query characters left
    }
    const int penalty = gap * kGapPenalty > kMaxGapPenalty ? kMaxGapPenalty : gap * kGapPenalty;
    score -= penalty;
    return score < 0 ? 0 : score;
}

}  // namespace krait::app::session
