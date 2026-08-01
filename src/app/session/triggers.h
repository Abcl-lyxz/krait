#pragma once

#include <cstddef>
#include <cstdint>
#include <regex>
#include <string>
#include <string_view>
#include <vector>

namespace krait::app::session {

// Triggers (T68) and snippets (T69): the two per-profile lists of "text the
// user cares about" — one matched against what the far end says, one sent when
// the user asks for it.
//
// WHY THIS IS Qt-FREE AND USES std::regex. rules/net.md: all remote input is
// hostile, and a regex engine fed attacker-controlled bytes is the classic way
// to hand a remote host your CPU. QRegularExpression is PCRE2, which
// backtracks, and Qt exposes NO way to bound it: MatchOption has exactly three
// values (NoMatchOption, AnchorAtOffsetMatchOption,
// DontCheckSubjectStringMatchOption) and nothing in the class touches PCRE2's
// match_limit or depth_limit (verified against the Qt 6.11 QRegularExpression
// class reference). MSVC's <regex> DOES bound itself — it throws
// regex_error(error_complexity) once a match exceeds its step budget, which is
// exactly what src/core/grid/search.cpp already relies on for the scrollback
// search. Same engine, same bound, one fewer thing to get right.
//
// That bound is the engine's; the caps below are ours, and both are needed:
// the complexity counter stops a pathological pattern, and the length caps stop
// an ordinary pattern being fed a megabyte.

struct TriggerActions {
    bool highlight = false;
    bool notify = false;
    bool log = false;
    // Text to send back to the session; empty means this trigger sends nothing.
    // Escapes (\r \n \t \\) are already decoded.
    std::string send;
};

struct Trigger {
    bool enabled = true;
    std::string pattern;
    bool caseSensitive = false;
    TriggerActions actions;
};

// One named piece of text the user can send (T69). Trusted in a way a
// trigger match is not — the user typed it — but it still goes out through the
// paste path, so it cannot become the one route that skips the ESC/C0 strip.
struct Snippet {
    std::string name;
    std::string text;
};

// The per-profile text format, one rule per line:
//
//     <pattern> >> <action>[,<action>]...
//
// Actions: `highlight`, `notify`, `log`, `case` (match case-sensitively;
// the default is insensitive), `off` (keep the rule, stop running it), and
// `send:<text>` which takes the whole rest of the line.
//
// The separator is " >> " and the split is on the LAST one, so a pattern that
// contains it still parses — the action list never does. Escaped as \> in a
// pattern if it ever matters, which it will not.
//
// A line format rather than an array of TOML tables because a Profile field is
// `key = text` all the way through (profile.cpp): inheritance from [defaults]
// and [folders."prod"], bulk edits and the round-trip all work on text, and a
// structured field would need its own copy of each. Write it as a TOML literal
// multi-line string ('''...''') so regex backslashes need no doubling.
std::vector<Trigger> parseTriggers(std::string_view text);
std::string triggersToText(const std::vector<Trigger>& triggers);

// Snippets, same shape: `<name> >> <text>`, split on the FIRST separator
// because here it is the tail that is free-form. Same escapes as `send:`.
std::vector<Snippet> parseSnippets(std::string_view text);
std::string snippetsToText(const std::vector<Snippet>& snippets);

// Text with its control characters written back as the escapes above, for
// showing a snippet's payload in the bar. A preview with an invisible CR in it
// is a preview that lies about what pressing the chip will do.
std::string escapeText(std::string_view text);

// What one trigger firing asks the app layer to do.
struct TriggerHit {
    std::size_t index = 0;  // into the trigger list the engine was given
    bool highlight = false;
    bool notify = false;
    bool log = false;
    // Already rate-limited and length-capped; empty means do not send.
    std::string send;
    // The matched text, REMOTE-controlled and capped. The app layer runs it
    // through net::sanitizeRemoteText before it reaches a banner or a log line
    // — this class stays Qt-free, and that helper is where the "remote text
    // next to trusted words" rule already lives.
    std::string matched;
};

// Matches an output stream against a profile's triggers.
//
// Chunk-oriented, because that is how output arrives. A match that straddles a
// chunk boundary is found by prepending the previous chunk's trailing partial
// line — BOUNDED (kMaxTail), because an unbounded carry-over is a memory leak
// a remote can drive by never sending a newline. Matches that end inside the
// carried tail were already reported last time and are dropped, so a straddling
// line fires exactly once.
class TriggerEngine {
  public:
    // A single match attempt never sees more than this. The engine's own
    // complexity counter bounds a pathological PATTERN; this bounds an ordinary
    // pattern fed a pathological amount of TEXT, which is the case a remote
    // controls directly.
    static constexpr std::size_t kMaxScan = 64 * 1024;
    // The carried-over partial line. Past this a match spanning the boundary is
    // missed rather than remembered forever.
    static constexpr std::size_t kMaxTail = 4 * 1024;
    // Per chunk, across all triggers. A screenful of matches is already more
    // than anyone reads; the rest is a remote buying UI work by the byte.
    static constexpr std::size_t kMaxHitsPerChunk = 64;
    // Rules past this are ignored. A config file is user input.
    static constexpr std::size_t kMaxTriggers = 64;
    static constexpr std::size_t kMaxMatchedChars = 200;

    // `send` limits. A trigger that sends is a remote-fired input primitive the
    // user pointed at themselves, so all three apply at once:
    static constexpr std::size_t kMaxSendBytes = 256;       // per send
    static constexpr std::uint64_t kSendIntervalMs = 2000;  // per trigger
    static constexpr int kSendBurst = 3;                    // per trigger, refilled
    static constexpr std::size_t kMaxSendsPerChunk = 1;     // across all triggers

    // Compiles `triggers`. A pattern that does not compile is DROPPED and named
    // in errors() — a broken rule must not take the other rules with it, and it
    // must not be silent either.
    //
    // `allowSend` is settings `triggers.allowSend`. False strips every send
    // action at compile time, so the dangerous action cannot fire because a
    // profile file said so; the user has to have opted in as well.
    void setTriggers(const std::vector<Trigger>& triggers, bool allowSend);

    const std::vector<std::string>& errors() const { return m_errors; }

    bool empty() const { return m_rules.empty(); }

    // Whether any rule wants highlighting, so the frame path can skip the whole
    // per-row scan when none does.
    bool hasHighlight() const { return m_anyHighlight; }

    // One chunk of output, already stripped of escape sequences (plainText()).
    // `nowMs` is a monotonic millisecond stamp from the caller — the same
    // arrangement Grid::nowMs uses, and for the same reason: this stays
    // testable without a clock.
    std::vector<TriggerHit> feed(std::string_view text, std::uint64_t nowMs);

    // Byte ranges of `text` that a `highlight` rule matches, half-open and
    // non-overlapping. Runs over ONE line, so a caller can ask per visible row
    // instead of storing coordinates that reflow would invalidate.
    void highlightRanges(std::string_view text,
                         std::vector<std::pair<std::size_t, std::size_t>>& out);

  private:
    struct Rule {
        std::size_t index = 0;
        std::regex expression;
        TriggerActions actions;
        // Absolute stream offset the next match of this rule must start at or
        // after. This is what makes a straddling match fire ONCE: the tail is
        // re-scanned every chunk, so a pattern with a trailing quantifier
        // (`err(or)?`, `error.*`) matches the same text again — longer — the
        // moment the rest of the line arrives. Deduplicating on where a match
        // ENDS misses that entirely, and the second fire lands in a different
        // chunk, so the per-chunk send cap does not catch it either.
        std::uint64_t nextOffset = 0;
        // The engine gave up on this rule: MSVC's <regex> hit its step budget.
        // Latched rather than retried, because highlightRanges() runs per
        // visible row per frame — retrying would buy the whole budget again on
        // every row, on the GUI thread, for as long as the pattern is loaded.
        bool dead = false;
        // Token bucket for `send`. A trigger that fires on its own echo is the
        // single most likely real-world failure, and this is what stops it
        // looping: the echo of the first send arrives inside the interval, so
        // the second send is refused.
        int tokens = kSendBurst;
        std::uint64_t refilledMs = 0;
    };

    // Spends one of `rule`'s send tokens, refilling first. False means the rule
    // has fired too recently and this send is dropped.
    static bool takeSendToken(Rule& rule, std::uint64_t nowMs);

    // Marks a rule dead and says so once. Non-const because the whole point is
    // that the state survives the chunk that provoked it.
    void retire(Rule& rule, const char* what);

    std::vector<Rule> m_rules;
    std::vector<std::string> m_errors;
    // The trailing partial line of the previous chunk, capped at kMaxTail.
    std::string m_tail;
    // Plain bytes fed so far. The absolute offset of `m_tail[0]` is
    // m_fed - m_tail.size(), which is what turns a match position inside one
    // chunk's subject into a stream position two chunks can compare.
    std::uint64_t m_fed = 0;
    bool m_anyHighlight = false;
};

// Where plainText() was when the previous chunk ran out. A remote chooses where
// its writes are cut, so a stripper that restarts at Ground on every read can be
// walked straight through: `ESC ]` in one packet and `0;error BEL` in the next
// would put the OSC payload into the match stream.
enum class StripState : std::uint8_t {
    Ground,
    Escape,  // ESC seen
    EscInt,  // intermediates of a plain escape
    Csi,     // CSI parameters and intermediates
    Str,     // OSC/DCS/SOS/PM/APC payload
    StrEsc,  // ESC inside a string payload — ST, or the string being abandoned
};

// Everything the far end sent, with the terminal control layer taken out:
// C0/C1 controls dropped, CSI/OSC/DCS sequences skipped, CRLF folded to \n.
//
// NOT a second VT parser — it drives no state and decides nothing about the
// screen. It exists so a pattern cannot be baited from inside an invisible
// escape payload, and so `^` means the start of a line rather than the start of
// whatever the last write happened to end with.
//
// It does follow the real parser on the two things that matter for that
// promise (tables.h): ESC is an anywhere-transition that CANCELS whatever was
// in progress, and a string is ended only by BEL or ST. Getting either wrong is
// a bypass, not a cosmetic difference.
std::string plainText(std::string_view raw, StripState& state);

// One-shot, for a caller with no stream to keep state for.
std::string plainText(std::string_view raw);

}  // namespace krait::app::session
