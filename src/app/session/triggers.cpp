#include "triggers.h"

#include <algorithm>

namespace krait::app::session {

namespace {

constexpr std::string_view kSeparator = " >> ";

// Per rule, per chunk. A chunk holding two hundred error lines is a chunk
// worth ONE look, not two hundred banner lines — but logging only the first of
// them would make the `log` action lie about how often something happened, so
// this is a cap rather than a limit of one.
constexpr std::size_t kMaxHitsPerRule = 8;

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

// `\r`, `\n`, `\t`, `\\` — and nothing else. A short list on purpose: this text
// is typed into a config file and read back by a human, and an escape nobody
// can remember is one nobody uses.
std::string decodeEscapes(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out += text[i];
            continue;
        }
        switch (text[i + 1]) {
        case 'r':
            out += '\r';
            break;
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case '\\':
            out += '\\';
            break;
        default:
            // Not an escape we know: both bytes stay, so a Windows path in a
            // snippet survives being written the way people actually write it.
            out += text[i];
            out += text[i + 1];
            break;
        }
        ++i;
    }
    return out;
}

}  // namespace

std::string escapeText(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char ch : text) {
        switch (ch) {
        case '\r':
            out += "\\r";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\\':
            out += "\\\\";
            break;
        default:
            out += ch;
            break;
        }
    }
    return out;
}

namespace {

// Every line of `text`, empty ones and `#` comments dropped.
std::vector<std::string_view> ruleLines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t newline = text.find('\n', start);
        const std::size_t end = newline == std::string_view::npos ? text.size() : newline;
        const std::string_view line = trim(text.substr(start, end - start));
        if (!line.empty() && line.front() != '#') {
            lines.push_back(line);
        }
        if (newline == std::string_view::npos) {
            break;
        }
        start = newline + 1;
    }
    return lines;
}

}  // namespace

std::vector<Trigger> parseTriggers(std::string_view text) {
    std::vector<Trigger> triggers;
    for (const std::string_view line : ruleLines(text)) {
        // The LAST separator: the action list never contains one, so splitting
        // there leaves whatever the pattern happens to hold on the pattern's
        // side rather than silently eating half of it.
        const std::size_t at = line.rfind(kSeparator);
        if (at == std::string_view::npos) {
            continue;  // no actions: a rule that does nothing is not a rule
        }
        Trigger trigger;
        trigger.pattern = std::string(trim(line.substr(0, at)));
        if (trigger.pattern.empty()) {
            continue;
        }

        std::string_view rest = trim(line.substr(at + kSeparator.size()));
        while (!rest.empty()) {
            // `send:` swallows the remainder of the line, so a sent command can
            // contain commas without anyone quoting anything.
            if (rest.starts_with("send:")) {
                trigger.actions.send = decodeEscapes(rest.substr(5));
                break;
            }
            const std::size_t comma = rest.find(',');
            const std::string_view token = trim(rest.substr(0, comma));
            if (token == "highlight") {
                trigger.actions.highlight = true;
            } else if (token == "notify") {
                trigger.actions.notify = true;
            } else if (token == "log") {
                trigger.actions.log = true;
            } else if (token == "case") {
                trigger.caseSensitive = true;
            } else if (token == "off") {
                trigger.enabled = false;
            }
            // An unknown token is ignored rather than failing the rule: a file
            // written by a newer Krait must not lose the actions this build
            // does understand.
            if (comma == std::string_view::npos) {
                break;
            }
            rest = trim(rest.substr(comma + 1));
        }
        triggers.push_back(std::move(trigger));
    }
    return triggers;
}

std::string triggersToText(const std::vector<Trigger>& triggers) {
    std::string out;
    for (const Trigger& trigger : triggers) {
        out += trigger.pattern;
        out += kSeparator;
        std::string actions;
        const auto add = [&actions](std::string_view token) {
            if (!actions.empty()) {
                actions += ',';
            }
            actions += token;
        };
        if (trigger.actions.highlight) {
            add("highlight");
        }
        if (trigger.actions.notify) {
            add("notify");
        }
        if (trigger.actions.log) {
            add("log");
        }
        if (trigger.caseSensitive) {
            add("case");
        }
        if (!trigger.enabled) {
            add("off");
        }
        if (!trigger.actions.send.empty()) {
            add("send:");
            actions += escapeText(trigger.actions.send);
        }
        out += actions;
        out += '\n';
    }
    return out;
}

std::vector<Snippet> parseSnippets(std::string_view text) {
    std::vector<Snippet> snippets;
    for (const std::string_view line : ruleLines(text)) {
        // The FIRST separator here, the mirror of parseTriggers: it is the tail
        // that is free-form in a snippet, and the name that is not.
        const std::size_t at = line.find(kSeparator);
        if (at == std::string_view::npos) {
            continue;
        }
        Snippet snippet;
        snippet.name = std::string(trim(line.substr(0, at)));
        snippet.text = decodeEscapes(trim(line.substr(at + kSeparator.size())));
        if (snippet.name.empty() || snippet.text.empty()) {
            continue;
        }
        snippets.push_back(std::move(snippet));
    }
    return snippets;
}

std::string snippetsToText(const std::vector<Snippet>& snippets) {
    std::string out;
    for (const Snippet& snippet : snippets) {
        out += snippet.name;
        out += kSeparator;
        out += escapeText(snippet.text);
        out += '\n';
    }
    return out;
}

std::string plainText(std::string_view raw, StripState& state) {
    std::string out;
    out.reserve(raw.size());

    // Index rather than a range-for: abandoning a string on ESC leaves the
    // CURRENT byte still to dispatch, in the escape state. Every other arm
    // advances, so the loop makes progress on at worst every second step.
    std::size_t i = 0;
    while (i < raw.size()) {
        const auto byte = static_cast<unsigned char>(raw[i]);
        switch (state) {
        case StripState::Ground:
            ++i;
            if (byte == 0x1B) {
                state = StripState::Escape;
            } else if (byte == '\n') {
                out += '\n';
            } else if (byte == '\t') {
                out += ' ';
            } else if (byte == '\r' || byte < 0x20 || byte == 0x7F) {
                // CR alone is a carriage return and CRLF is one line ending;
                // the LF is what ends a line here. Every other C0, and DEL,
                // simply goes.
            } else {
                // 0x80-0xFF passes through: under UTF-8 those are lead and
                // continuation bytes, not C1 controls, and mangling them would
                // break every pattern a Thai or CJK user writes.
                out += static_cast<char>(byte);
            }
            break;

        case StripState::Escape:
            ++i;
            if (byte == 0x1B) {
                break;  // ESC ESC: still in escape, which is what the parser does
            }
            if (byte == '[') {
                state = StripState::Csi;
            } else if (byte == ']' || byte == 'P' || byte == 'X' || byte == '^' || byte == '_') {
                state = StripState::Str;
            } else if (byte >= 0x20 && byte <= 0x2F) {
                state = StripState::EscInt;
            } else {
                state = StripState::Ground;  // the final byte of a short escape
            }
            break;

        case StripState::EscInt:
            ++i;
            if (byte == 0x1B) {
                state = StripState::Escape;
            } else if (byte < 0x20 || byte > 0x2F) {
                state = StripState::Ground;  // the final byte
            }
            break;

        case StripState::Csi:
            ++i;
            if (byte == 0x1B) {
                // ESC anywhere CANCELS the sequence in progress (tables.h sets
                // this transition for every state). Treating it as just another
                // parameter byte is how `ESC [ 0 ESC ] 0 ; bait BEL` would put
                // the OSC payload on the match stream.
                state = StripState::Escape;
            } else if (byte >= 0x40 && byte <= 0x7E) {
                state = StripState::Ground;  // ECMA-48 5.4: the final byte
            }
            break;

        case StripState::Str:
            ++i;
            if (byte == 0x07) {
                state = StripState::Ground;  // BEL
            } else if (byte == 0x1B) {
                state = StripState::StrEsc;
            }
            break;

        case StripState::StrEsc:
            if (byte == '\\') {
                ++i;
                state = StripState::Ground;  // ST
                break;
            }
            // ESC then anything else abandons the string and re-enters escape
            // with THIS byte still to dispatch — deliberately not consumed.
            state = StripState::Escape;
            break;
        }
    }
    return out;
}

std::string plainText(std::string_view raw) {
    StripState state = StripState::Ground;
    return plainText(raw, state);
}

void TriggerEngine::setTriggers(const std::vector<Trigger>& triggers, bool allowSend) {
    m_rules.clear();
    m_errors.clear();
    m_tail.clear();
    m_fed = 0;
    m_anyHighlight = false;

    for (std::size_t i = 0; i < triggers.size() && m_rules.size() < kMaxTriggers; ++i) {
        const Trigger& trigger = triggers[i];
        if (!trigger.enabled || trigger.pattern.empty()) {
            continue;
        }
        Rule rule;
        rule.index = i;
        rule.actions = trigger.actions;
        if (!allowSend) {
            // Settings `triggers.allowSend` is off. Stripped HERE rather than
            // at the send site so there is exactly one place the answer can be
            // yes, and a profile file alone can never be it.
            rule.actions.send.clear();
        }
        try {
            auto flags = std::regex::ECMAScript | std::regex::optimize | std::regex::multiline;
            if (!trigger.caseSensitive) {
                flags |= std::regex::icase;
            }
            rule.expression = std::regex(trigger.pattern, flags);
        } catch (const std::regex_error& error) {
            // A broken rule must not take the working ones with it, and it must
            // not be silent either — the caller puts errors() in a banner.
            m_errors.push_back(trigger.pattern + ": " + error.what());
            continue;
        }
        m_anyHighlight = m_anyHighlight || rule.actions.highlight;
        m_rules.push_back(std::move(rule));
    }
}

void TriggerEngine::retire(Rule& rule, const char* what) {
    if (rule.dead) {
        return;
    }
    rule.dead = true;
    m_errors.emplace_back(std::string("a trigger pattern was too expensive to run: ") + what);
}

std::vector<TriggerHit> TriggerEngine::feed(std::string_view text, std::uint64_t nowMs) {
    std::vector<TriggerHit> hits;
    if (m_rules.empty() || text.empty()) {
        return hits;
    }

    // The carried tail first, then as much of the chunk as the scan cap allows.
    // Anything past the cap in a SINGLE chunk is not scanned — a documented
    // bound, and the one that keeps a 10 MB burst from being a 10 MB regex run
    // on the UI thread.
    const std::size_t tailLen = m_tail.size();
    const std::string_view scanned = text.substr(0, kMaxScan);
    const bool truncated = scanned.size() < text.size();
    std::string subject = m_tail;
    subject.append(scanned);
    // Absolute stream offset of subject[0], fixed before m_fed moves on.
    const std::uint64_t base = m_fed - tailLen;
    m_fed += scanned.size();

    std::size_t sends = 0;
    for (Rule& rule : m_rules) {
        if (rule.dead) {
            continue;
        }
        std::size_t perRule = 0;
        // Matching is inside the try as well as construction: MSVC's <regex>
        // throws regex_error(error_complexity) while MATCHING, not while
        // compiling — `(a+)+$` against a line of a's is enough — so a try
        // around only the constructor would let it escape into a Qt slot and
        // become std::terminate. This is the bound that makes a hostile input
        // cost a bounded number of steps instead of the process.
        try {
            for (auto it = std::sregex_iterator(subject.begin(), subject.end(), rule.expression);
                 it != std::sregex_iterator(); ++it) {
                if (perRule >= kMaxHitsPerRule || hits.size() >= kMaxHitsPerChunk) {
                    break;
                }
                const auto begin = static_cast<std::size_t>(it->position(0));
                const auto length = static_cast<std::size_t>(it->length(0));
                if (length == 0) {
                    continue;  // `^` alone would otherwise hit once per position
                }
                // Deduplicated on where the match BEGINS in the stream, not on
                // where it ends: the tail is re-scanned every chunk, so
                // `err(or)?` matching "err" and then "error" is the same match
                // growing, not a second one.
                const std::uint64_t absolute = base + begin;
                if (absolute < rule.nextOffset) {
                    continue;
                }
                rule.nextOffset = absolute + 1;

                TriggerHit hit;
                hit.index = rule.index;
                hit.highlight = rule.actions.highlight;
                hit.notify = rule.actions.notify;
                hit.log = rule.actions.log;
                hit.matched = subject.substr(begin, std::min(length, kMaxMatchedChars));

                if (!rule.actions.send.empty() && perRule == 0 && sends < kMaxSendsPerChunk &&
                    takeSendToken(rule, nowMs)) {
                    hit.send = rule.actions.send.substr(0, kMaxSendBytes);
                    ++sends;
                }
                hits.push_back(std::move(hit));
                ++perRule;
            }
        } catch (const std::regex_error& error) {
            // The engine hit its step budget. The rule is retired rather than
            // retried: the alternative is buying the whole budget again on
            // every chunk the remote chooses to send, on the GUI thread. The
            // other rules still run, and errors() says which one went.
            retire(rule, error.what());
            continue;
        }
    }

    if (truncated) {
        // The scan cap dropped the end of this chunk, so whatever the next one
        // starts with did NOT follow this tail on the wire. Gluing them would
        // let a match — including a send — fire on an adjacency that never
        // existed.
        m_tail.clear();
        m_fed += text.size() - scanned.size();
        return hits;
    }

    // Carry the trailing partial line, capped. Unbounded, this is a memory leak
    // a remote drives by never sending a newline.
    const std::size_t lastNewline = subject.rfind('\n');
    std::string_view partial = lastNewline == std::string::npos
                                   ? std::string_view(subject)
                                   : std::string_view(subject).substr(lastNewline + 1);
    if (partial.size() > kMaxTail) {
        partial = partial.substr(partial.size() - kMaxTail);
    }
    m_tail.assign(partial);
    return hits;
}

bool TriggerEngine::takeSendToken(Rule& rule, std::uint64_t nowMs) {
    if (nowMs >= rule.refilledMs + kSendIntervalMs) {
        const std::uint64_t steps = (nowMs - rule.refilledMs) / kSendIntervalMs;
        rule.tokens = std::min<int>(
            kSendBurst, rule.tokens + static_cast<int>(std::min<std::uint64_t>(steps, kSendBurst)));
        rule.refilledMs += steps * kSendIntervalMs;
    }
    if (rule.tokens <= 0) {
        return false;
    }
    --rule.tokens;
    return true;
}

void TriggerEngine::highlightRanges(std::string_view text,
                                    std::vector<std::pair<std::size_t, std::size_t>>& out) {
    out.clear();
    if (!m_anyHighlight || text.empty()) {
        return;
    }
    // ONE row, capped. This runs per visible row per frame, so the cap is what
    // keeps a 100k-column line from being a regex run at presentation rate.
    const std::string line(text.substr(0, kMaxScan));
    for (Rule& rule : m_rules) {
        if (!rule.actions.highlight || rule.dead) {
            continue;
        }
        try {
            for (auto it = std::sregex_iterator(line.begin(), line.end(), rule.expression);
                 it != std::sregex_iterator(); ++it) {
                if (out.size() >= kMaxHitsPerChunk) {
                    return;
                }
                const auto begin = static_cast<std::size_t>(it->position(0));
                const auto length = static_cast<std::size_t>(it->length(0));
                if (length > 0) {
                    out.emplace_back(begin, begin + length);
                }
            }
        } catch (const std::regex_error& error) {
            // Same bound as feed(), and the same latch — this is the call site
            // where retrying would be worst, since it runs once per visible row
            // per frame.
            retire(rule, error.what());
            continue;
        }
    }
    std::sort(out.begin(), out.end());
}

}  // namespace krait::app::session
