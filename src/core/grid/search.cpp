#include "core/grid/search.h"

#include "core/grid/cell.h"
#include "core/grid/cluster_pool.h"
#include "core/grid/grid.h"
#include "core/grid/line.h"

#include <cctype>
#include <regex>

namespace krait::core::vt {

namespace {

void appendUtf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | cp >> 6);
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | cp >> 12);
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | cp >> 18);
        out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
        out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

// Word bytes for the fallback selection. '-' and '_' are in, because
// `some-package_name` is one thing to anyone reading it; '.' and '/' are NOT,
// because they belong to the path and URL rules that run first.
bool isWordByte(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return byte >= 0x80 ||  // any non-ASCII: word-ness is not ours to judge
           (byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
           (byte >= 'A' && byte <= 'Z') || byte == '_' || byte == '-';
}

bool isBoundary(char ch) {
    return ch == ' ' || ch == '\t' || ch == '"' || ch == '\'' || ch == '<' || ch == '>' ||
           ch == '(' || ch == ')' || ch == '[' || ch == ']' || ch == '{' || ch == '}' ||
           ch == '`' || ch == ',';
}

// The run of non-boundary bytes containing `at`.
SmartSelection runAround(std::string_view text, std::size_t at) {
    std::size_t begin = at;
    while (begin > 0 && !isBoundary(text[begin - 1])) {
        --begin;
    }
    std::size_t end = at;
    while (end < text.size() && !isBoundary(text[end])) {
        ++end;
    }
    return {begin, end};
}

std::string lowered(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

}  // namespace

std::string lineText(const Line& line, const ClusterPool& pool, std::vector<int>* columns) {
    // Find the last written cell first, so the padding a fixed-width row always
    // carries never reaches the output.
    std::size_t last = line.cells.size();
    while (last > 0 && line.cells[last - 1].ch == 0) {
        --last;
    }

    std::string out;
    out.reserve(last);
    if (columns != nullptr) {
        columns->clear();
        columns->reserve(last + 1);
    }
    // Every byte appended is charged to the cell that produced it, so the two
    // stay in step even where one cell contributes several bytes (a Thai
    // syllable) or none (the trailing half of a wide cluster).
    const auto charge = [&](std::size_t cell) {
        if (columns != nullptr) {
            columns->resize(out.size(), static_cast<int>(cell));
        }
    };

    for (std::size_t i = 0; i < last; ++i) {
        const char32_t ch = line.cells[i].ch;
        if (ch == 0) {
            // An interior hole an application punched with CUP+EL. A space
            // keeps the column alignment that makes a match offset mean
            // something.
            out += ' ';
            charge(i);
            continue;
        }
        if (isWideTrailing(ch)) {
            // The right half of a 2-column cluster: the same character as its
            // lead, which was already emitted.
            continue;
        }
        if (isClusterRef(ch)) {
            for (const char32_t cp : pool.lookup(ch)) {
                appendUtf8(out, cp);
            }
            charge(i);
            continue;
        }
        appendUtf8(out, ch);
        charge(i);
    }
    if (columns != nullptr) {
        // One past the end, so a half-open byte range [b, e) maps to the
        // half-open column range [(*columns)[b], (*columns)[e]) with no special
        // case at the last byte.
        columns->push_back(static_cast<int>(last));
    }
    return out;
}

std::expected<std::vector<SearchHit>, std::string>
searchScrollback(const Grid& grid, std::string_view pattern, const SearchOptions& options) {
    if (pattern.empty()) {
        return std::vector<SearchHit>{};
    }

    // Built ONCE, outside the loop: constructing a std::regex is expensive
    // enough that doing it per line turns a search into a visible pause.
    std::regex expression;
    if (options.regex) {
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize;
            if (!options.caseSensitive) {
                flags |= std::regex::icase;
            }
            expression = std::regex(std::string(pattern), flags);
        } catch (const std::regex_error& error) {
            // A search box is user input and half-typed patterns are the normal
            // case, so this is a value, not a crash.
            return std::unexpected(std::string(error.what()));
        }
    }

    const std::string needle = options.caseSensitive ? std::string(pattern) : lowered(pattern);
    std::vector<SearchHit> hits;

    const auto findIn = [&](const std::string& text, std::size_t lineIndex) {
        if (options.regex) {
            for (auto it = std::sregex_iterator(text.begin(), text.end(), expression);
                 it != std::sregex_iterator(); ++it) {
                if (hits.size() >= options.maxHits) {
                    return;
                }
                // A zero-width match (`^`, an empty alternation) would otherwise
                // produce one hit per position and fill the cap with nothing.
                if (it->length(0) == 0) {
                    continue;
                }
                hits.push_back({lineIndex, static_cast<std::size_t>(it->position(0)),
                                static_cast<std::size_t>(it->position(0) + it->length(0))});
            }
            return;
        }
        const std::string haystack = options.caseSensitive ? text : lowered(text);
        std::size_t at = haystack.find(needle);
        while (at != std::string::npos && hits.size() < options.maxHits) {
            hits.push_back({lineIndex, at, at + needle.size()});
            at = haystack.find(needle, at + needle.size());
        }
    };

    // The MATCHING is inside the try as well as the construction. MSVC's
    // <regex> throws regex_error(error_complexity) while matching, not while
    // compiling — `(a+)+$` against a line of thirty a's is enough — so a try
    // that covers only the constructor leaves an exception escaping src/core
    // into a Qt slot, where it becomes std::terminate. The header promises
    // nothing escapes; this is what makes that true.
    try {
        std::size_t lineIndex = 0;
        for (std::size_t i = 0; i < grid.scrollbackSize(); ++i, ++lineIndex) {
            findIn(lineText(grid.scrollbackAt(i), grid.clusters()), lineIndex);
            if (hits.size() >= options.maxHits) {
                return hits;
            }
        }
        for (const Line& row : grid.viewportRows()) {
            findIn(lineText(row, grid.clusters()), lineIndex++);
            if (hits.size() >= options.maxHits) {
                return hits;
            }
        }
    } catch (const std::regex_error& error) {
        return std::unexpected(std::string(error.what()));
    }
    return hits;
}

SmartSelection smartSelect(std::string_view text, std::size_t at) {
    if (text.empty()) {
        return {0, 0};
    }
    if (at >= text.size()) {
        at = text.size() - 1;
    }

    const SmartSelection run = runAround(text, at);
    const std::string_view token = text.substr(run.begin, run.end - run.begin);

    // A URL wins, and it wins WHOLE. Taking only the hostname out of one is the
    // classic terminal annoyance: the thing on screen is one thing, and a
    // double-click that hands back two thirds of it is one you learn not to use.
    for (const std::string_view scheme : {"https://", "http://", "ssh://", "ftp://", "file://"}) {
        const std::size_t schemeAt = token.find(scheme);
        if (schemeAt == std::string_view::npos) {
            continue;
        }
        std::size_t end = run.end;
        // Trailing punctuation belongs to the sentence, not the URL — a link at
        // the end of a log line almost always has a period after it.
        while (end > run.begin + schemeAt) {
            const char last = text[end - 1];
            if (last == '.' || last == ',' || last == ';' || last == ':' || last == '!' ||
                last == '?') {
                --end;
                continue;
            }
            break;
        }
        return {run.begin + schemeAt, end};
    }

    // A path: the whole run, if it looks like one. Windows and POSIX both.
    if (token.find('/') != std::string_view::npos || token.find('\\') != std::string_view::npos) {
        return run;
    }

    // Otherwise a word. This is where '.' stops being part of the token, so
    // double-clicking `example.test` in prose takes one word — the path rule
    // above is what makes `./a.txt` behave differently, which is the
    // distinction people actually expect.
    if (!isWordByte(text[at])) {
        // Sitting on punctuation or a space: take just that byte rather than
        // guessing which neighbour was meant.
        return {at, at + 1};
    }
    std::size_t begin = at;
    while (begin > 0 && isWordByte(text[begin - 1])) {
        --begin;
    }
    std::size_t end = at;
    while (end < text.size() && isWordByte(text[end])) {
        ++end;
    }
    return {begin, end};
}

}  // namespace krait::core::vt
