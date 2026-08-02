// T68 — the trigger regression suite docs/plan/01-milestones.md names in M4's
// acceptance criteria.
//
// The security-shaped cases are the point of this file, not an extra: a regex
// engine fed attacker-controlled bytes is how a remote host takes your CPU, and
// a trigger that sends text back is an input primitive the remote fires. Both
// are timed or counted here rather than argued about in a comment.

#include "triggers.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

using krait::app::session::parseSnippets;
using krait::app::session::parseTriggers;
using krait::app::session::plainText;
using krait::app::session::Snippet;
using krait::app::session::snippetsToText;
using krait::app::session::Trigger;
using krait::app::session::TriggerEngine;
using krait::app::session::TriggerHit;
using krait::app::session::triggersToText;

namespace {

TriggerEngine engineFor(std::string_view config, bool allowSend = true) {
    TriggerEngine engine;
    engine.setTriggers(parseTriggers(config), allowSend);
    return engine;
}

}  // namespace

TEST_CASE("a trigger line parses into a pattern and its actions", "[triggers]") {
    const auto triggers = parseTriggers(R"(
# a comment, and the blank line above

\berror\b >> highlight,notify,log
Are you sure >> send:yes\r
PANIC >> highlight,case
stale >> highlight,off
)");
    REQUIRE(triggers.size() == 4);

    CHECK(triggers[0].pattern == "\\berror\\b");
    CHECK(triggers[0].actions.highlight);
    CHECK(triggers[0].actions.notify);
    CHECK(triggers[0].actions.log);
    CHECK(triggers[0].actions.send.empty());
    CHECK_FALSE(triggers[0].caseSensitive);

    // `send:` takes the whole rest of the line, so a command with a comma in it
    // needs no quoting, and \r is decoded to a real carriage return.
    CHECK(triggers[1].actions.send == "yes\r");

    CHECK(triggers[2].caseSensitive);
    CHECK_FALSE(triggers[3].enabled);
}

TEST_CASE("a trigger round-trips through its text form", "[triggers]") {
    // The profile stores text (profile.cpp is `key = text` all the way through),
    // so a rule that cannot survive a save is a rule the session editor loses.
    const std::string original = "\\berror\\b >> highlight,notify\nAre you sure >> send:yes\\r\n";
    const std::string again = triggersToText(parseTriggers(original));
    CHECK(again == original);
}

TEST_CASE("a pattern containing the separator still parses", "[triggers]") {
    // The split is on the LAST " >> ": the action list never contains one, so
    // whatever the pattern holds stays on the pattern's side.
    const auto triggers = parseTriggers("a >> b >> highlight");
    REQUIRE(triggers.size() == 1);
    CHECK(triggers[0].pattern == "a >> b");
    CHECK(triggers[0].actions.highlight);
}

TEST_CASE("plainText strips the control layer", "[triggers]") {
    // Matching raw bytes would let a remote bait a pattern from inside an
    // escape payload it knows will never be drawn.
    CHECK(plainText("\x1B]0;error in the title\x07ok") == "ok");
    CHECK(plainText("\x1B[31mred\x1B[0m") == "red");
    CHECK(plainText("a\r\nb") == "a\nb");
    CHECK(plainText("\x1B(Bplain") == "plain");
    // Non-ASCII passes through untouched: under UTF-8 those bytes are lead and
    // continuation bytes, not C1 controls, and mangling them would break every
    // pattern a Thai or CJK user writes.
    CHECK(plainText("ผิดพลาด") == "ผิดพลาด");
}

TEST_CASE("plainText follows the real parser where a bypass would live", "[triggers]") {
    using krait::app::session::StripState;

    // ESC is an anywhere-transition that CANCELS what was in progress
    // (tables.h). Treating the byte after ESC as a final, or any 0x40-0x7E as a
    // CSI final, is a bypass: the OSC payload lands in the match stream and a
    // pattern fires on text the user can never see.
    CHECK(plainText("\x1B\x1B]0;bait\x07ok") == "ok");
    CHECK(plainText("\x1B[0\x1B]0;bait\x07ok") == "ok");
    // ST ends a string; an ESC inside one that is not ST abandons it, and the
    // byte after is dispatched as an escape rather than printed.
    CHECK(plainText("\x1B]0;t\x1B\\ok") == "ok");
    CHECK(plainText("\x1B]0;t\x1B[31mok") == "ok");

    // Split across reads, which a remote chooses. A stripper that restarts at
    // Ground on every chunk can be walked straight through.
    StripState state = StripState::Ground;
    CHECK(plainText("\x1B]0;ba", state).empty());
    CHECK(plainText("it\x07ok", state) == "ok");

    // And an interrupted sequence leaves the state where the next chunk needs
    // it rather than dumping the remainder as text.
    state = StripState::Ground;
    CHECK(plainText("live\x1B[", state) == "live");
    CHECK(plainText("31mred", state) == "red");
}

TEST_CASE("a trigger fires on the output it matches", "[triggers]") {
    auto engine = engineFor("error >> notify,log\n");
    const auto hits = engine.feed(plainText("\x1B[31mdisk error: sector 5\r\n"), 0);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].notify);
    CHECK(hits[0].log);
    CHECK(hits[0].matched == "error");
    CHECK(hits[0].send.empty());
}

TEST_CASE("case sensitivity is per rule", "[triggers]") {
    CHECK(engineFor("ERROR >> notify\n").feed("an error happened", 0).size() == 1);
    CHECK(engineFor("ERROR >> notify,case\n").feed("an error happened", 0).empty());
}

TEST_CASE("a match straddling a chunk boundary fires exactly once", "[triggers]") {
    auto engine = engineFor("connection refused >> notify\n");

    // Split mid-phrase, with no newline in the first chunk: the trailing partial
    // line is carried, so the match is found on the second chunk.
    CHECK(engine.feed("ssh: connection ref", 0).empty());
    const auto hits = engine.feed("used\n", 10);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].matched == "connection refused");

    // And NOT again: the carried tail after a newline is empty, so the third
    // chunk sees only its own text.
    CHECK(engine.feed("something else\n", 20).empty());
}

TEST_CASE("a match that GROWS across a chunk boundary still fires once", "[triggers]") {
    // The dedupe has to be on where a match BEGINS. The tail is re-scanned
    // every chunk, so any pattern with a trailing quantifier matches the same
    // text again — longer — the moment the rest of the line arrives. Checking
    // where a match ends misses that entirely, and because the two fires land
    // in different chunks, the per-chunk send cap does not catch it either.
    auto engine = engineFor("err(or)? >> notify\n");
    REQUIRE(engine.feed("an err", 0).size() == 1);
    CHECK(engine.feed("or here\n", 10).empty());

    // Same shape with a send attached: the duplicate would have been a second
    // command run on the far end, funded by the burst allowance.
    auto sender = engineFor("Continue.* >> send:y\\r\n");
    REQUIRE_FALSE(sender.feed("Continue", 0)[0].send.empty());
    CHECK(sender.feed(" [y/N]?\n", 10).empty());
}

TEST_CASE("a match already reported inside the carried tail does not repeat", "[triggers]") {
    auto engine = engineFor("ready >> notify\n");
    // No newline, so the whole chunk is carried forward.
    REQUIRE(engine.feed("system ready", 0).size() == 1);
    // The next chunk prepends that tail. The match sits entirely inside it and
    // was reported last time, so it must not come back.
    CHECK(engine.feed(" and waiting\n", 10).empty());
}

TEST_CASE("the carried tail is bounded", "[triggers]") {
    auto engine = engineFor("nomatch >> notify\n");
    // A remote that never sends a newline: without a cap this grows forever,
    // which is a memory leak driven from the far end.
    const std::string flood(TriggerEngine::kMaxTail * 4, 'x');
    for (int i = 0; i < 8; ++i) {
        engine.feed(flood, static_cast<std::uint64_t>(i));
    }
    // Nothing to assert on the tail from outside, so assert on the behaviour it
    // bounds: a pattern anchored to the start of the accumulated text can no
    // longer see the beginning of it.
    auto anchored = engineFor("^start >> notify\n");
    anchored.feed("start", 0);
    const std::string filler(TriggerEngine::kMaxTail + 16, 'y');
    CHECK(anchored.feed(filler, 10).empty());
}

TEST_CASE("send is refused unless the user opted in", "[triggers]") {
    // settings triggers.allowSend, off by default. A profile file arriving from
    // somewhere else must not be able to turn its own send actions on.
    auto refused = engineFor("prompt >> send:secret\r\n", /*allowSend=*/false);
    const auto hits = refused.feed("prompt\n", 0);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].send.empty());

    auto allowed = engineFor("prompt >> send:ok\n", /*allowSend=*/true);
    CHECK_FALSE(allowed.feed("prompt\n", 0)[0].send.empty());
}

TEST_CASE("send is rate-limited so a trigger cannot loop on its own echo", "[triggers]") {
    auto engine = engineFor("go >> send:go\\r\n");

    // The burst is spendable, one send per chunk.
    int sent = 0;
    for (int i = 0; i < 10; ++i) {
        for (const TriggerHit& hit : engine.feed("go\n", 0)) {
            if (!hit.send.empty()) {
                ++sent;
            }
        }
    }
    // Time never advances, so only the initial burst may go out — this is the
    // echo loop, and it stops after three rather than running forever.
    CHECK(sent == TriggerEngine::kSendBurst);

    // One interval later, exactly one more token is available.
    sent = 0;
    for (int i = 0; i < 10; ++i) {
        for (const TriggerHit& hit : engine.feed("go\n", TriggerEngine::kSendIntervalMs)) {
            if (!hit.send.empty()) {
                ++sent;
            }
        }
    }
    CHECK(sent == 1);
}

TEST_CASE("at most one send leaves per chunk", "[triggers]") {
    // Three rules that all match the same line. Without the per-chunk cap, one
    // line of remote output would fire three commands at once.
    auto engine = engineFor("a >> send:1\nb >> send:2\nc >> send:3\n");
    int sends = 0;
    for (const TriggerHit& hit : engine.feed("abc\n", 0)) {
        if (!hit.send.empty()) {
            ++sends;
        }
    }
    CHECK(sends == static_cast<int>(TriggerEngine::kMaxSendsPerChunk));
}

TEST_CASE("the sent text is capped", "[triggers]") {
    const std::string huge(TriggerEngine::kMaxSendBytes * 4, 'z');
    auto engine = engineFor("go >> send:" + huge + "\n");
    const auto hits = engine.feed("go\n", 0);
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].send.size() == TriggerEngine::kMaxSendBytes);
}

TEST_CASE("a broken pattern is skipped and named, not fatal", "[triggers]") {
    auto engine = engineFor("[unclosed >> notify\ngood >> notify\n");
    CHECK(engine.errors().size() == 1);
    // The working rule still runs: one bad line in a config file must not
    // silently disable the rest of it.
    CHECK(engine.feed("a good line\n", 0).size() == 1);
}

TEST_CASE("the matched text handed back is capped", "[triggers]") {
    auto engine = engineFor(".+ >> notify\n");
    const std::string line(TriggerEngine::kMaxMatchedChars * 10, 'q');
    const auto hits = engine.feed(line + "\n", 0);
    REQUIRE_FALSE(hits.empty());
    CHECK(hits[0].matched.size() == TriggerEngine::kMaxMatchedChars);
}

TEST_CASE("hostile input against a pathological pattern is bounded", "[triggers]") {
    // THE case this whole design is shaped around. `(a+)+$` against a run of
    // a's with no trailing match is the textbook catastrophic-backtracking
    // input: a naive engine explores 2^n paths.
    //
    // Qt's QRegularExpression is PCRE2 and exposes no way to bound that — its
    // MatchOption enum has exactly three values and none of them is a match or
    // depth limit (Qt 6.11 class reference). MSVC's <regex> DOES bound itself:
    // it throws regex_error(error_complexity) once a match exceeds its step
    // budget, which the engine catches. That is why triggers use std::regex,
    // and this is the test that says the bound is real rather than assumed.
    auto engine = engineFor("(a+)+$ >> notify\n");
    const std::string hostile(2000, 'a');

    const auto started = std::chrono::steady_clock::now();
    engine.feed(hostile + "b\n", 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();

    // Generous on purpose — this is a CI machine under load, and the assertion
    // that matters is "bounded", not a millisecond figure. Unbounded
    // backtracking on this input does not finish this century.
    INFO("elapsed ms: " << elapsed);
    CHECK(elapsed < 2000);
}

TEST_CASE("a long hostile line is not scanned without limit", "[triggers]") {
    // The engine's complexity counter bounds a pathological PATTERN. This is
    // the other half: an ordinary pattern fed a pathological amount of text,
    // which is the case a remote controls directly.
    auto engine = engineFor("needle >> notify\n");
    const std::string flood(std::size_t{4} * 1024 * 1024, 'x');

    const auto started = std::chrono::steady_clock::now();
    engine.feed(flood, 0);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - started)
                             .count();
    INFO("elapsed ms: " << elapsed);
    CHECK(elapsed < 2000);
}

TEST_CASE("highlight ranges come back as byte offsets into the row", "[triggers]") {
    auto engine = engineFor("err(or)? >> highlight\nother >> notify\n");
    CHECK(engine.hasHighlight());

    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    engine.highlightRanges("an error and an err here", ranges);
    REQUIRE(ranges.size() == 2);
    CHECK(ranges[0] == std::pair<std::size_t, std::size_t>{3, 8});
    CHECK(ranges[1] == std::pair<std::size_t, std::size_t>{16, 19});

    // A rule without the highlight action contributes nothing, so the frame
    // path cannot paint over text no rule asked for.
    engine.highlightRanges("other", ranges);
    CHECK(ranges.empty());
}

TEST_CASE("no highlight rule means no per-row work at all", "[triggers]") {
    auto engine = engineFor("error >> notify\n");
    CHECK_FALSE(engine.hasHighlight());
    std::vector<std::pair<std::size_t, std::size_t>> ranges;
    engine.highlightRanges("error", ranges);
    CHECK(ranges.empty());
}

TEST_CASE("snippets parse, round-trip and keep their escapes", "[triggers]") {
    const auto snippets = parseSnippets("Disk usage >> df -h\\r\nTail log >> tail -f a.log\n");
    REQUIRE(snippets.size() == 2);
    CHECK(snippets[0].name == "Disk usage");
    CHECK(snippets[0].text == "df -h\r");
    CHECK(snippets[1].text == "tail -f a.log");

    // The FIRST separator here, the mirror of a trigger: it is the tail that is
    // free-form in a snippet.
    const auto arrowed = parseSnippets("Push >> git push >> origin\n");
    REQUIRE(arrowed.size() == 1);
    CHECK(arrowed[0].name == "Push");
    CHECK(arrowed[0].text == "git push >> origin");

    CHECK(snippetsToText(snippets) == "Disk usage >> df -h\\r\nTail log >> tail -f a.log\n");
}

TEST_CASE("a snippet with no text or no name is dropped", "[triggers]") {
    // Half a rule is not a rule. Sending an empty snippet would look like the
    // bar had done nothing, which is worse than it not being there.
    CHECK(parseSnippets("Name >> \n >> text\nno separator\n").empty());
}
