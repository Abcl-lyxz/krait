#include "core/parser/osc.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace krait::core::vt;

namespace {

// Feeds bytes through a real Session and collects the OSC actions it produced.
std::vector<OscAction> run(Session& session, std::string_view bytes) {
    std::vector<OscAction> actions;
    session.onOsc = [&actions](const OscAction& action) { actions.push_back(action); };
    session.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
    return actions;
}

void feed(Session& session, std::string_view bytes) {
    session.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

}  // namespace

TEST_CASE("base64 round-trips and refuses what it cannot decode", "[core][osc]") {
    for (const std::string_view sample : {"", "a", "ab", "abc", "hello world", "\x01\x02\xff"}) {
        std::string decoded;
        REQUIRE(decodeBase64(encodeBase64(sample), &decoded, 1024));
        CHECK(decoded == sample);
    }

    std::string out;
    // Strict on purpose. Skipping whitespace would make two terminals disagree
    // about what a payload decodes to, and clipboard contents are exactly the
    // thing that must not silently differ.
    CHECK_FALSE(decodeBase64("aGVs bG8=", &out, 1024));
    CHECK_FALSE(decodeBase64("not base64!", &out, 1024));
    // A single leftover character encodes no whole byte: the sender is confused
    // or hostile either way.
    CHECK_FALSE(decodeBase64("aGVsbG8x2", &out, 1024));
    // Over the cap is refused rather than truncated.
    CHECK_FALSE(decodeBase64(encodeBase64(std::string(2048, 'x')), &out, 64));
    CHECK(out.empty());
}

TEST_CASE("OSC 8 carries the URI and its id", "[core][osc]") {
    Session session(24, 80);

    const std::vector<OscAction> opened = run(session, "\x1b]8;id=42;https://example.test/a\x1b\\");
    REQUIRE(opened.size() == 1);
    CHECK(opened[0].kind == OscAction::Kind::Hyperlink);
    CHECK(opened[0].text == "https://example.test/a");
    CHECK(opened[0].id == "42");

    // An empty URI closes the link rather than opening one to nowhere.
    const std::vector<OscAction> closed = run(session, "\x1b]8;;\x1b\\");
    REQUIRE(closed.size() == 1);
    CHECK(closed[0].kind == OscAction::Kind::Hyperlink);
    CHECK(closed[0].text.empty());

    // An unknown parameter is skipped, not treated as a failure — the spec
    // requires a terminal to ignore what it does not recognise.
    const std::vector<OscAction> extra =
        run(session, "\x1b]8;future=x:id=7;https://example.test/b\x1b\\");
    REQUIRE(extra.size() == 1);
    CHECK(extra[0].id == "7");
    CHECK(extra[0].text == "https://example.test/b");
}

TEST_CASE("OSC 52 write decodes, read needs permission", "[core][osc]") {
    Session session(24, 80);

    const std::vector<OscAction> write = run(session, "\x1b]52;c;aGVsbG8=\x1b\\");
    REQUIRE(write.size() == 1);
    CHECK(write[0].kind == OscAction::Kind::ClipboardWrite);
    CHECK(write[0].text == "hello");
    CHECK(write[0].selection == "c");

    // The security case. With any program on the remote host able to ask, a
    // read answered by default hands over whatever the user last copied —
    // possibly a password for a different machine.
    CHECK_FALSE(session.clipboardReadAllowed());
    CHECK(run(session, "\x1b]52;c;?\x1b\\").empty());

    session.allowClipboardRead(true);
    const std::vector<OscAction> read = run(session, "\x1b]52;c;?\x1b\\");
    REQUIRE(read.size() == 1);
    CHECK(read[0].kind == OscAction::Kind::ClipboardRead);
}

TEST_CASE("a hostile OSC string cannot grow the terminal", "[core][osc]") {
    Session session(24, 80);

    // Well past the payload cap. The parser has no buffer of its own, so this
    // handler is the only thing between a server and unbounded memory.
    std::string huge = "\x1b]52;c;";
    huge += std::string(OscHandler::kMaxPayload * 2, 'A');
    huge += "\x1b\\";
    CHECK(run(session, huge).empty());

    // A truncated payload is refused OUTRIGHT rather than acted on in part:
    // half a URI is a different URI, and half a clipboard is worse than none.
    std::string longUri = "\x1b]8;;https://example.test/";
    longUri += std::string(OscHandler::kMaxPayload, 'p');
    longUri += "\x1b\\";
    CHECK(run(session, longUri).empty());

    // The session keeps working afterwards — an overflow must not wedge the
    // handler for everything that follows it.
    const std::vector<OscAction> after = run(session, "\x1b]52;c;aGk=\x1b\\");
    REQUIRE(after.size() == 1);
    CHECK(after[0].text == "hi");
}

TEST_CASE("an aborted or unknown OSC does nothing", "[core][osc]") {
    Session session(24, 80);

    // CAN aborts the string. xterm discards those and so do we.
    CHECK(run(session, "\x1b]52;c;aGVsbG8=\x18").empty());
    // Codes we do not implement stay silent rather than half-handled. OSC 7
    // (cwd) stands in for the family here; this line used to be OSC 4, which
    // T83 implemented — leaving it would have asserted the opposite of what the
    // terminal now does, and it is the suite noticing that which is the point.
    CHECK(run(session, "\x1b]7;file://host/tmp\x1b\\").empty());
    // An OSC 133 with a letter nobody implements marks nothing and reports
    // nothing — the same honest silence as OSC 4 above. (A is a different
    // story: it marks the grid AND reports, which the shell-integration case
    // below pins.)
    CHECK(run(session, "\x1b]133;Z\x1b\\").empty());
    // An empty string is not an action.
    CHECK(run(session, "\x1b]\x1b\\").empty());
}

TEST_CASE("OSC 0 and 2 are titles", "[core][osc]") {
    Session session(24, 80);
    const std::vector<OscAction> title = run(session, "\x1b]0;my shell\x1b\\");
    REQUIRE(title.size() == 1);
    CHECK(title[0].kind == OscAction::Kind::Title);
    CHECK(title[0].text == "my shell");
}

TEST_CASE("OSC 9;4 reports taskbar progress and refuses what it cannot read", "[core][osc]") {
    Session session(24, 80);

    // State 1 carries a percentage.
    const std::vector<OscAction> set = run(session, "\x1b]9;4;1;42\x1b\\");
    REQUIRE(set.size() == 1);
    CHECK(set[0].kind == OscAction::Kind::Progress);
    CHECK(set[0].progress == OscAction::Progress::Set);
    CHECK(set[0].percent == 42);

    // BEL terminates it as well as ST, and 0 removes the bar.
    const std::vector<OscAction> gone = run(session, "\x1b]9;4;0\x07");
    REQUIRE(gone.size() == 1);
    CHECK(gone[0].progress == OscAction::Progress::Remove);

    // 2 and 4 may omit the percentage — ConEmu says so in as many words — and
    // "absent" stays distinct from 0 so the app can leave the bar where it was.
    for (const auto& [bytes, state] :
         {std::pair{"\x1b]9;4;2\x1b\\", OscAction::Progress::Error},
          std::pair{"\x1b]9;4;4\x1b\\", OscAction::Progress::Paused}}) {
        const std::vector<OscAction> partial = run(session, bytes);
        REQUIRE(partial.size() == 1);
        CHECK(partial[0].progress == state);
        CHECK(partial[0].percent == -1);
    }

    // 3 is indeterminate; Microsoft says the value is ignored, so whether one
    // arrives or not the state is what matters.
    const std::vector<OscAction> marquee = run(session, "\x1b]9;4;3;77\x1b\\");
    REQUIRE(marquee.size() == 1);
    CHECK(marquee[0].progress == OscAction::Progress::Indeterminate);

    // Out of range is CLAMPED rather than refused: neither MS nor ConEmu says
    // what 250% means, and a bar pinned at full is the only reading of it that
    // is not a lie. (A refusal would leave a stale bar saying something else.)
    const std::vector<OscAction> over = run(session, "\x1b]9;4;1;250\x1b\\");
    REQUIRE(over.size() == 1);
    CHECK(over[0].percent == 100);

    // Hostile / confused input is refused WHOLE. Every one of these is a sender
    // that does not mean what a legal string means, and guessing the nearest
    // legal state would put something on the user's taskbar nobody asked for.
    for (const std::string_view bad : {
             "\x1b]9;4;9;50\x1b\\",          // no such state
             "\x1b]9;4;10;50\x1b\\",         // two digits is not a state, even if 1 is
             "\x1b]9;5;1;50\x1b\\",          // OSC 9 subcommand we do not implement
             "\x1b]9;4x;1\x1b\\",            // subcommand is not exactly "4"
             "\x1b]9;a notification\x1b\\",  // ConEmu's OSC 9 message form
             "\x1b]9\x1b\\",
         }) {
        CAPTURE(bad);
        CHECK(run(session, bad).empty());
    }

    // DEFAULT PARAMETER. An absent or empty state is REMOVE, not malformed:
    // Windows Terminal's own dispatcher reads a missing state as 0, and
    // refusing it would leave a bar asserting progress with nothing able to
    // clear it. `;;50` is the same shape with a percentage after it.
    for (const std::string_view clears :
         {"\x1b]9;4\x1b\\", "\x1b]9;4;\x1b\\", "\x1b]9;4;;50\x1b\\"}) {
        CAPTURE(clears);
        const std::vector<OscAction> off = run(session, clears);
        REQUIRE(off.size() == 1);
        CHECK(off[0].progress == OscAction::Progress::Remove);
    }

    // State 1 with no percentage at all: the state is legal, the figure simply
    // is not there. Distinct from 0, so the app can pick a default per state.
    const std::vector<OscAction> bare = run(session, "\x1b]9;4;1\x1b\\");
    REQUIRE(bare.size() == 1);
    CHECK(bare[0].progress == OscAction::Progress::Set);
    CHECK(bare[0].percent == -1);

    // Both ends of the documented 0-100 range are inclusive.
    for (const auto& [bytes, want] :
         {std::pair{"\x1b]9;4;1;0\x1b\\", 0}, std::pair{"\x1b]9;4;1;100\x1b\\", 100},
          std::pair{"\x1b]9;4;1;101\x1b\\", 100}}) {
        CAPTURE(bytes);
        const std::vector<OscAction> at = run(session, bytes);
        REQUIRE(at.size() == 1);
        CHECK(at[0].percent == want);
    }

    // An overlong percentage is refused as a PERCENTAGE, not as a state: "137"
    // truncated to "13" would be a different answer, not a rounded one.
    const std::vector<OscAction> huge = run(session, "\x1b]9;4;1;99999999999999999999\x1b\\");
    REQUIRE(huge.size() == 1);
    CHECK(huge[0].percent == -1);

    // INTERRUPTED mid-payload. CAN and SUB abort the string (xterm discards
    // those and so do we); ESC is the first half of ST, so the machine leaves
    // the string on it and dispatches what it had. Either way the parser comes
    // back clean, which the sequence after the loop proves.
    for (const std::string_view aborted : {"\x1b]9;4;1;50\x18", "\x1b]9;4;1;50\x1a"}) {
        CAPTURE(aborted);
        CHECK(run(session, aborted).empty());
    }
    const std::vector<OscAction> introduced = run(session, "\x1b]9;4;1;50\x1b[c");
    REQUIRE(introduced.size() == 1);
    CHECK(introduced[0].percent == 50);
    CHECK(run(session, "\x1b]9;4;1;7\x1b\\").size() == 1);

    // SPLIT ACROSS CHUNKS, at every byte. A socket read cutting an OSC in half
    // is the normal case, not the exotic one — the parser holds no buffer of
    // its own, so OscHandler has to accumulate across feeds or nothing works.
    for (const std::string_view whole : {"\x1b]9;4;1;42\x1b\\", "\x1b]9;4;2\x07"}) {
        CAPTURE(whole);
        for (std::size_t cut = 1; cut < whole.size(); ++cut) {
            CAPTURE(cut);
            Session split(24, 80);
            std::vector<OscAction> seen;
            split.onOsc = [&seen](const OscAction& one) { seen.push_back(one); };
            feed(split, whole.substr(0, cut));
            feed(split, whole.substr(cut));
            REQUIRE(seen.size() == 1);
            CHECK(seen[0].kind == OscAction::Kind::Progress);
        }
    }

    // A percentage that is not a number is absent, not fatal: the STATE is the
    // part the sender got right, and dropping it would lose a real error bar.
    const std::vector<OscAction> junk = run(session, "\x1b]9;4;2;oops\x1b\\");
    REQUIRE(junk.size() == 1);
    CHECK(junk[0].progress == OscAction::Progress::Error);
    CHECK(junk[0].percent == -1);

    // Positional, like OSC 133 ; D's status: a later field that happens to be a
    // number is an option, not the percentage.
    const std::vector<OscAction> positional = run(session, "\x1b]9;4;1;oops;50\x1b\\");
    REQUIRE(positional.size() == 1);
    CHECK(positional[0].percent == -1);

    // Nothing here ever answers the remote side.
    std::string replies;
    session.onReply = [&replies](const std::string& out) { replies += out; };
    feed(session, "\x1b]9;4;1;10\x1b\\\x1b]9;4;0\x1b\\");
    CHECK(replies.empty());
}

TEST_CASE("OSC 133 marks the grid and never answers the remote side", "[core][osc][shell]") {
    Session session(6, 20);
    std::string replies;
    session.onReply = [&replies](const std::string& out) { replies += out; };
    int oscActions = 0;
    session.onOsc = [&oscActions](const OscAction&) { ++oscActions; };

    feed(session, "\x1b]133;A;aid=7;cl=m\x1b\\user@host$ \x1b]133;B\x1b\\ls\r\n"
                  "\x1b]133;C;cmdline_url=ls\x1b\\file-a\r\n"
                  "\x1b]133;D;3\x1b\\");

    CHECK(replies.empty());
    // The mark is applied by the core AND forwarded: A, B, C and D each report
    // once. The app needs the C -> D pair because it is the only signal that a
    // command ran and for how long, and src/core/ may not read a clock
    // (rules/vt-core.md). What it must never do is REPLY, which is the line
    // above.
    CHECK(oscActions == 4);

    const Grid& grid = session.grid();
    CHECK((grid.absoluteLineAt(0).marks & kMarkPromptStart) != 0);
    CHECK((grid.absoluteLineAt(0).marks & kMarkInputStart) != 0);
    CHECK((grid.absoluteLineAt(1).marks & kMarkOutputStart) != 0);
    CHECK((grid.absoluteLineAt(2).marks & kMarkCommandEnd) != 0);
    // The status lands on the line the command was TYPED on, not on the line D
    // happened to arrive at.
    CHECK(grid.absoluteLineAt(0).exitCode == 3);
    CHECK(grid.absoluteLineAt(2).exitCode == -1);
}

TEST_CASE("OSC 133 D writes its status onto a prompt already in scrollback", "[core][osc][shell]") {
    // Why the status cannot live in a side table keyed by row: by the time a
    // slow command finishes, the line it was typed on has usually left the
    // screen entirely.
    Session session(3, 20);
    feed(session, "\x1b]133;A\x1b\\$ sleep\r\n\x1b]133;C\x1b\\");
    feed(session, "a\r\nb\r\nc\r\nd\r\ne\r\n");

    const Grid& grid = session.grid();
    REQUIRE(grid.scrollbackSize() > 0);
    const std::optional<std::size_t> prompt = grid.prevPrompt(grid.absoluteLineCount());
    REQUIRE(prompt.has_value());
    REQUIRE(*prompt < grid.scrollbackSize());  // it really did retire into history

    feed(session, "\x1b]133;D;130\x1b\\");
    CHECK(grid.absoluteLineAt(*prompt).exitCode == 130);
}

TEST_CASE("prompt navigation walks scrollback and screen as one list", "[core][osc][shell]") {
    Session session(4, 20);
    for (const std::string_view command : {"one", "two", "three"}) {
        feed(session, "\x1b]133;A\x1b\\$ ");
        feed(session, command);
        feed(session, "\r\n\x1b]133;C\x1b\\out\r\n");
    }

    const Grid& grid = session.grid();
    std::vector<std::size_t> prompts;
    // nextPrompt is strictly-after, so seed the walk with line 0 when the very
    // first line is itself a prompt. That asymmetry is what makes a
    // jump-to-prompt binding pressed twice move twice.
    if ((grid.absoluteLineAt(0).marks & kMarkPromptStart) != 0) {
        prompts.push_back(0);
    }
    for (std::optional<std::size_t> at = grid.nextPrompt(0); at; at = grid.nextPrompt(*at)) {
        prompts.push_back(*at);
    }
    REQUIRE(prompts.size() == 3);
    CHECK(prompts[0] < prompts[1]);
    CHECK(prompts[1] < prompts[2]);
    // The first of the three has retired into history; the walk crossed the
    // scrollback/screen seam without the caller knowing there was one.
    CHECK(prompts[0] < grid.scrollbackSize());
    CHECK(prompts[2] >= grid.scrollbackSize());

    CHECK(grid.prevPrompt(prompts[2]) == prompts[1]);
    CHECK(grid.prevPrompt(prompts[1]) == prompts[0]);
    CHECK_FALSE(grid.prevPrompt(prompts[0]).has_value());
    CHECK_FALSE(grid.nextPrompt(prompts[2]).has_value());
    // Out-of-range starts are answers, not crashes: a UI asks from wherever the
    // viewport happens to be, and the viewport outlives the lines it names.
    CHECK(grid.prevPrompt(grid.absoluteLineCount() * 4) == prompts[2]);
    CHECK_FALSE(grid.nextPrompt(grid.absoluteLineCount() * 4).has_value());
    CHECK_FALSE(grid.nextPrompt(static_cast<std::size_t>(-1)).has_value());
}

TEST_CASE("OSC 133 marks the head of a wrapped prompt, not the cursor's row",
          "[core][osc][shell]") {
    // B arrives after a prompt that has already wrapped, so the cursor sits on
    // a continuation row. The mark must walk back to the head, or the next
    // resize moves it.
    Session session(4, 8);
    feed(session, "\x1b]133;A\x1b\\0123456789ab\x1b]133;B\x1b\\");

    const Grid& grid = session.grid();
    REQUIRE(grid.absoluteLineAt(1).wrappedFromPrev);
    CHECK((grid.absoluteLineAt(0).marks & kMarkInputStart) != 0);
    CHECK(grid.absoluteLineAt(1).marks == 0);
}

TEST_CASE("OSC 133 D attributes the status to the FIRST prompt of a multi-line command",
          "[core][osc][shell]") {
    // kitty sends `A;k=s` before PS2. Treating that as a prompt start would put
    // the status on the last continuation row instead of the line the command
    // began on — the exact failure the write-it-back design exists to prevent.
    Session session(6, 20);
    feed(session, "\x1b]133;A\x1b\\$ for i\r\n");
    feed(session, "\x1b]133;A;k=s\x1b\\> do\r\n");
    feed(session, "\x1b]133;C\x1b\\out\r\n\x1b]133;D;2\x1b\\");

    const Grid& grid = session.grid();
    CHECK(grid.absoluteLineAt(0).exitCode == 2);
    CHECK(grid.absoluteLineAt(1).exitCode == -1);
    CHECK(grid.absoluteLineAt(1).marks == 0);  // PS2 is not a jump target
    CHECK_FALSE(grid.nextPrompt(0).has_value());
}

TEST_CASE("OSC 133 P is prompt start, which is what wezterm and zsh emit", "[core][osc][shell]") {
    // wezterm's shipped integration and zsh's own Src/Zle/termquery.c send
    // `133;P;k=i`, never `133;A`. Handling only A means zero marks for them.
    Session session(4, 20);
    feed(session, "\x1b]133;P;k=i\x1b\\$ \x1b]133;B\x1b\\");
    const Grid& grid = session.grid();
    CHECK((grid.absoluteLineAt(0).marks & kMarkPromptStart) != 0);
    CHECK((grid.absoluteLineAt(0).marks & kMarkInputStart) != 0);

    Session secondary(4, 20);
    feed(secondary, "\x1b]133;P;k=s\x1b\\");
    CHECK(secondary.grid().absoluteLineAt(0).marks == 0);
}

TEST_CASE("erase takes prompt marks with the text they described", "[core][osc][shell]") {
    // Otherwise jump-to-prompt lands on blank rows after a `clear`, and the
    // resize blank-row absorption stops seeing those rows as spare.
    Session session(6, 20);
    feed(session, "\x1b]133;A\x1b\\$ x\r\nout\r\n");
    REQUIRE(session.grid().prevPrompt(session.grid().absoluteLineCount()).has_value());

    feed(session, "\x1b[H\x1b[2J");
    CHECK_FALSE(session.grid().prevPrompt(session.grid().absoluteLineCount()).has_value());

    // And the cleared rows are absorbable again, which is what a narrowing
    // resize needs so it does not retire live content into history.
    session.grid().resize(3, 10);
    CHECK(session.grid().scrollbackSize() == 0);
}

TEST_CASE("shell integration is a no-op on the alternate screen", "[core][osc][shell]") {
    // m_scrollback is the NORMAL buffer's history, so a mark placed while a
    // full-screen app owns the viewport would name a line in the wrong buffer.
    Session session(4, 20);
    feed(session, "\x1b[?1049h\x1b]133;A\x1b\\\x1b]133;D;1\x1b\\");
    const Grid& grid = session.grid();
    CHECK_FALSE(grid.prevPrompt(grid.absoluteLineCount()).has_value());
    feed(session, "\x1b[?1049l");
    CHECK_FALSE(grid.prevPrompt(grid.absoluteLineCount()).has_value());
}

TEST_CASE("a D with no prompt open does no history walk at all", "[core][osc][shell]") {
    // The bound on a hostile stream: `\e]133;D\e\\` is 13 bytes, and without
    // this a megabyte of them would walk all of scrollback per sequence.
    // Observable as behaviour rather than as timing: the second D of a pair
    // cannot claim the prompt the first one already closed.
    Session session(4, 20);
    feed(session, "\x1b]133;A\x1b\\$ x\r\n\x1b]133;D;7\x1b\\");
    const Grid& grid = session.grid();
    const std::optional<std::size_t> prompt = grid.prevPrompt(grid.absoluteLineCount());
    REQUIRE(prompt.has_value());
    CHECK(grid.absoluteLineAt(*prompt).exitCode == 7);

    feed(session, "\x1b]133;D;9\x1b\\");
    CHECK(grid.absoluteLineAt(*prompt).exitCode == 7);  // not re-claimed
}

TEST_CASE("a flood of A + 2J + D triples stays bounded instead of rescanning history",
          "[core][osc][shell]") {
    // The hostile case T66's `m_promptOpen` bool did NOT cover, and the reason
    // the open prompt is a stable index now. `ESC]133;A ST` `CSI 2J`
    // `ESC]133;D ST` is 25 bytes: the 2J clears the mark (sgr.cpp) without
    // closing the prompt, so a walk bounded only by "is a prompt open" searches
    // ALL of history, finds nothing, and does it again for every 25 bytes.
    // With the floor the walk stops where the A opened, which no amount of
    // remote output can push further away for free.
    // Small on purpose. `CSI 2J` is O(rows x cols) by definition and that work
    // is legitimate — it is the HISTORY walk this case is about, and a walk
    // costs the same per line at any width. A 24x80 grid buries a 10x
    // regression under its own honest clearing; 6x20 leaves it in the open.
    Session session(6, 20);

    // A full ring first, so a regression has a whole history to rescan. This is
    // the multiplier: with the bool alone the flood below is ~10^8 line visits.
    std::string history;
    for (int i = 0; i < 12'000; ++i) {
        history += "line\r\n";
    }
    feed(session, history);
    REQUIRE(session.grid().scrollbackSize() > 9'000);

    std::string flood;
    for (int i = 0; i < 40'000; ++i) {
        flood += "\x1b]133;A\x1b\\\x1b[2J\x1b]133;D;1\x1b\\";
    }

    const auto started = std::chrono::steady_clock::now();
    feed(session, flood);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);

    // A hard bound, so a regression is a FAILING ASSERTION rather than a ctest
    // that never returns and a CI job killed by its own timeout with nothing to
    // read (the pattern tests/unit/sftp_test.cpp's flood case uses). Generous
    // against a loaded machine and still two orders of magnitude under the
    // unbounded walk, which is minutes here.
    UNSCOPED_INFO("40k A/2J/D triples over " << session.grid().scrollbackSize()
                                             << " history lines took " << elapsed.count() << " ms");
    CHECK(elapsed < std::chrono::seconds(5));

    // And the bound did not come from refusing to do the work: a D that follows
    // a REAL, uncleared A still finds it and still writes the status.
    Session live(4, 20);
    feed(live, "\x1b]133;A\x1b\\$ x\r\nout\r\n\x1b]133;D;42\x1b\\");
    const std::optional<std::size_t> prompt =
        live.grid().prevPrompt(live.grid().absoluteLineCount());
    REQUIRE(prompt.has_value());
    CHECK(live.grid().absoluteLineAt(*prompt).exitCode == 42);
}

TEST_CASE("an open prompt survives the eviction of everything around it", "[core][osc][shell]") {
    // The other half of the same change: the floor is a STABLE index, so the
    // bound must not turn into a wrong answer when the ring evicts underneath
    // it. A row index here would name different text after the first eviction.
    Session session(3, 20);
    session.grid().scrollback().setCaps(8, 4'000);

    feed(session, "\x1b]133;A\x1b\\$ slow\r\n\x1b]133;C\x1b\\");
    // Comfortably more output than the 8-line ring holds, so the prompt line is
    // evicted along with everything else while the command is still running.
    for (int i = 0; i < 40; ++i) {
        feed(session, "out\r\n");
    }
    REQUIRE(session.grid().scrollbackSize() <= 8);
    REQUIRE_FALSE(session.grid().prevPrompt(session.grid().absoluteLineCount()).has_value());

    // The prompt it belonged to is gone, so there is nothing to write and
    // nothing is invented — in particular no status lands on whichever line
    // happens to sit at the old index now.
    feed(session, "\x1b]133;D;7\x1b\\");
    for (std::size_t i = 0; i < session.grid().absoluteLineCount(); ++i) {
        CHECK(session.grid().absoluteLineAt(i).exitCode == -1);
    }
}

TEST_CASE("OSC 133 refuses a status it cannot trust", "[core][osc][shell]") {
    for (const std::string_view bad :
         {"\x1b]133;D;-1\x1b\\", "\x1b]133;D;oops\x1b\\", "\x1b]133;D;99999999999\x1b\\",
          "\x1b]133;D;2147483648\x1b\\", "\x1b]133;D;\x1b\\", "\x1b]133;D\x1b\\"}) {
        CAPTURE(bad);
        Session session(3, 20);
        feed(session, "\x1b]133;A\x1b\\$\r\n");
        feed(session, bad);
        // The mark still lands — the command DID finish, we just do not know
        // how — but no status is invented for it.
        CHECK((session.grid().absoluteLineAt(1).marks & kMarkCommandEnd) != 0);
        CHECK(session.grid().absoluteLineAt(0).exitCode == -1);
    }

    // 2147483647 is the largest value the bound accepts, and it is accepted.
    Session session(3, 20);
    feed(session, "\x1b]133;A\x1b\\$\r\n\x1b]133;D;2147483647\x1b\\");
    CHECK(session.grid().absoluteLineAt(0).exitCode == 2147483647);
}

// ---------------------------------------------------------------------------
// OSC 4/10/11/12 and 104/110/111/112 — colour set, query and reset (M5 T83).
//
// The core deliberately does NOT parse the colour spec. It owns no palette, so
// a parser here would be a second XParseColor reader that could disagree with
// the one reading theme files, about a value the core cannot use. What it does
// own is deciding WHICH colour was named and WHETHER the sender was asking or
// telling — and that is what these assert.

TEST_CASE("OSC 10/11/12 set the dynamic colours", "[core][osc][color]") {
    Session session(4, 10);

    for (const auto& [code, slot] : {std::pair{"10", OscAction::ColorSlot::Foreground},
                                     std::pair{"11", OscAction::ColorSlot::Background},
                                     std::pair{"12", OscAction::ColorSlot::Cursor}}) {
        const auto actions = run(session, "\x1b]" + std::string(code) + ";#ff8800\x1b\\");
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].kind == OscAction::Kind::ColorSet);
        CHECK(actions[0].slot == slot);
        // Verbatim, not normalised: the app parses it, and normalising here
        // would mean two readers that can disagree about the same bytes.
        CHECK(actions[0].text == "#ff8800");
    }

    // XParseColor's form reaches the app untouched too.
    const auto xparse = run(session, "\x1b]11;rgb:1e/1e/2e\x1b\\");
    REQUIRE(xparse.size() == 1);
    CHECK(xparse[0].text == "rgb:1e/1e/2e");
}

TEST_CASE("OSC 10/11/12 with '?' are questions, not settings", "[core][osc][color]") {
    Session session(4, 10);
    const auto actions = run(session, "\x1b]11;?\x1b\\");
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].kind == OscAction::Kind::ColorQuery);
    CHECK(actions[0].slot == OscAction::ColorSlot::Background);
    // A query carries no value. Reporting one would invite the app to apply it.
    CHECK(actions[0].text.empty());
}

TEST_CASE("OSC 4 names one palette entry", "[core][osc][color]") {
    Session session(4, 10);

    const auto set = run(session, "\x1b]4;12;#89b4fa\x1b\\");
    REQUIRE(set.size() == 1);
    CHECK(set[0].kind == OscAction::Kind::ColorSet);
    CHECK(set[0].slot == OscAction::ColorSlot::Palette);
    CHECK(set[0].colorIndex == 12);
    CHECK(set[0].text == "#89b4fa");

    const auto query = run(session, "\x1b]4;0;?\x1b\\");
    REQUIRE(query.size() == 1);
    CHECK(query[0].kind == OscAction::Kind::ColorQuery);
    CHECK(query[0].colorIndex == 0);

    // 255 is the last legal entry; 256 is not an entry at all.
    const auto last = run(session, "\x1b]4;255;#000000\x1b\\");
    REQUIRE(last.size() == 1);
    CHECK(last[0].colorIndex == 255);
    CHECK(run(session, "\x1b]4;256;#000000\x1b\\").empty());
    CHECK(run(session, "\x1b]4;-1;#000000\x1b\\").empty());
    // Digits only. "12x" must not be read as 12 — a colour applied to the
    // wrong entry because a parser stopped early is worse than one not applied.
    CHECK(run(session, "\x1b]4;12x;#000000\x1b\\").empty());
    CHECK(run(session, "\x1b]4;;#000000\x1b\\").empty());
    // An index with no spec at all names nothing to do.
    CHECK(run(session, "\x1b]4;12\x1b\\").empty());
    CHECK(run(session, "\x1b]4;12;\x1b\\").empty());
}

TEST_CASE("a colour list reports its first pair and drops the rest", "[core][osc][color]") {
    Session session(4, 10);
    // xterm honours `OSC 4;1;red;2;green` in full. One OscAction carries one
    // action, so the remainder is DROPPED rather than misapplied — the safe
    // direction for a colour, and nothing in the wild sends more than a pair.
    const auto actions = run(session, "\x1b]4;1;#ff0000;2;#00ff00\x1b\\");
    REQUIRE(actions.size() == 1);
    CHECK(actions[0].colorIndex == 1);
    CHECK(actions[0].text == "#ff0000");

    const auto dynamic = run(session, "\x1b]10;#ffffff;#000000\x1b\\");
    REQUIRE(dynamic.size() == 1);
    CHECK(dynamic[0].slot == OscAction::ColorSlot::Foreground);
    CHECK(dynamic[0].text == "#ffffff");
}

TEST_CASE("OSC 104/110/111/112 reset colours", "[core][osc][color]") {
    Session session(4, 10);

    // A BARE 104 resets the whole palette; -1 is what says so, and it is
    // distinct from 0, which is one entry. The distinction IS the sequence.
    const auto all = run(session, "\x1b]104\x1b\\");
    REQUIRE(all.size() == 1);
    CHECK(all[0].kind == OscAction::Kind::ColorReset);
    CHECK(all[0].slot == OscAction::ColorSlot::Palette);
    CHECK(all[0].colorIndex == -1);

    const auto one = run(session, "\x1b]104;7\x1b\\");
    REQUIRE(one.size() == 1);
    CHECK(one[0].kind == OscAction::Kind::ColorReset);
    CHECK(one[0].colorIndex == 7);

    // A bad index resets NOTHING rather than everything. Reading
    // "OSC 104 ; garbage" as the bare form would wipe a palette the sender was
    // trying to touch one entry of.
    CHECK(run(session, "\x1b]104;nope\x1b\\").empty());

    for (const auto& [code, slot] : {std::pair{"110", OscAction::ColorSlot::Foreground},
                                     std::pair{"111", OscAction::ColorSlot::Background},
                                     std::pair{"112", OscAction::ColorSlot::Cursor}}) {
        const auto actions = run(session, "\x1b]" + std::string(code) + "\x1b\\");
        REQUIRE(actions.size() == 1);
        CHECK(actions[0].kind == OscAction::Kind::ColorReset);
        CHECK(actions[0].slot == slot);
    }
}

TEST_CASE("colour sequences never make the core answer", "[core][osc][color]") {
    Session session(4, 10);
    // The reply to a query is the APP's — it is the only layer that knows the
    // palette. If the core ever answered, the two would eventually disagree,
    // and a terminal reporting a colour it is not drawing is worse than one
    // that says nothing.
    std::string replies;
    session.onReply = [&replies](const std::string& text) { replies += text; };
    run(session, "\x1b]11;?\x1b\\x1b]4;0;?\x1b\\x1b]10;#fff\x1b\\x1b]104\x1b\\");
    CHECK(replies.empty());
}

TEST_CASE("malformed and interrupted colour sequences produce nothing", "[core][osc][color]") {
    Session session(4, 10);
    // Aborted mid-string: xterm discards these and so do we.
    CHECK(run(session, "\x1b]11;#ff0000\x18").empty());
    CHECK(run(session, "\x1b]4;1;#ff0000\x1a").empty());
    // Empty payloads.
    CHECK(run(session, "\x1b]11\x1b\\").empty());
    CHECK(run(session, "\x1b]11;\x1b\\").empty());
    CHECK(run(session, "\x1b]4\x1b\\").empty());
    // Codes that merely start with ours are NOT ours: 1000 is not 10, and 41
    // is not 4. A prefix match here would let an unimplemented sequence repaint
    // the window.
    CHECK(run(session, "\x1b]1000;#ff0000\x1b\\").empty());
    CHECK(run(session, "\x1b]41;1;#ff0000\x1b\\").empty());
    CHECK(run(session, "\x1b]113;#ff0000\x1b\\").empty());

    // Over the payload cap: refused outright rather than acted on in part.
    std::string huge = "\x1b]11;#";
    huge.append(OscHandler::kMaxPayload + 16, 'a');
    huge += "\x1b\\";
    CHECK(run(session, huge).empty());
}

TEST_CASE("a colour sequence may end with BEL as well as ST", "[core][osc][color]") {
    Session session(4, 10);
    const auto bel = run(session, "\x1b]11;#123456\x07");
    REQUIRE(bel.size() == 1);
    CHECK(bel[0].kind == OscAction::Kind::ColorSet);
    CHECK(bel[0].text == "#123456");
}

TEST_CASE("a truncated colour sequence stays open until the next ESC", "[core][osc][color]") {
    // Not a curiosity — it is how the parser is specified, and getting it wrong
    // is how a test "proves" a malformed sequence was ignored while the real
    // terminal applies it one chunk later. In the DEC state machine an ESC
    // arriving inside an OSC string ENDS that string (it is assumed to begin
    // ST), so an unterminated payload is delivered when the next sequence
    // starts rather than discarded.
    Session session(4, 10);

    // Nothing yet: the string has not ended.
    CHECK(run(session, "\x1b]11;#ff0000").empty());

    // The ESC that opens the NEXT sequence closes the previous one, so this
    // feed yields two actions — the deferred one first.
    const auto actions = run(session, "\x1b]10;#00ff00\x1b\\");
    REQUIRE(actions.size() == 2);
    CHECK(actions[0].slot == OscAction::ColorSlot::Background);
    CHECK(actions[0].text == "#ff0000");
    CHECK(actions[1].slot == OscAction::ColorSlot::Foreground);
    CHECK(actions[1].text == "#00ff00");
}

// ---------------------------------------------------------------------------
// OSC 66 — kitty's text-sizing protocol (M5 T81).

TEST_CASE("OSC 66 writes text at a scale", "[core][osc][sizing]") {
    Session session(4, 40);
    feed(session, "\x1b]66;s=2;Hi\x1b\\");

    const Grid& grid = session.grid();
    // The text went through the ORDINARY print path, so it is on the grid as
    // cells — selectable, copyable, and reflowable like any other text.
    CHECK(grid.cellAt(0, 0).ch == U'H');
    CHECK(grid.cellAt(0, 1).ch == U'i');
    // The scale rides in the spare bits of Attr::flags, so a sized cell costs
    // nothing extra in scrollback.
    CHECK(grid.cellAt(0, 0).attr.scale() == 2);
    CHECK(grid.cellAt(0, 1).attr.scale() == 2);

    // And the pen is RESTORED: text after the sequence is normal size again.
    feed(session, "x");
    CHECK(grid.cellAt(0, 2).attr.scale() == 1);
}

TEST_CASE("an unscaled cell reads as scale 1 without anything setting it", "[core][osc][sizing]") {
    // Every cell written before T81 existed, and every cell written by ordinary
    // text, has zero in those bits. Reading zero as "scale 0" would make the
    // renderer divide by it.
    Session session(4, 10);
    feed(session, "abc");
    CHECK(session.grid().cellAt(0, 0).attr.scale() == 1);
    CHECK(session.grid().cellAt(0, 0).attr.sizeWidth() == 0);
}

TEST_CASE("OSC 66 metadata is COLON separated", "[core][osc][sizing]") {
    // The one thing to get wrong here: every other OSC in the file separates
    // with ';', and ';' separates the metadata from the TEXT. Reading colons as
    // semicolons truncates the payload at the first field.
    Session session(4, 40);
    feed(session, "\x1b]66;s=3:w=2;AB\x1b\\");
    CHECK(session.grid().cellAt(0, 0).ch == U'A');
    CHECK(session.grid().cellAt(0, 0).attr.scale() == 3);
    CHECK(session.grid().cellAt(0, 0).attr.sizeWidth() == 2);
    // w != 0 means "render all of it in s*w cells", so the cursor advances by
    // exactly that — 3 * 2 = 6 — whatever the text measured.
    CHECK(session.grid().col == 6);
}

TEST_CASE("OSC 66 with no metadata still writes the text", "[core][osc][sizing]") {
    Session session(4, 20);
    feed(session, "\x1b]66;;plain\x1b\\");
    CHECK(session.grid().cellAt(0, 0).ch == U'p');
    CHECK(session.grid().cellAt(0, 0).attr.scale() == 1);
    CHECK(session.grid().col == 5);
}

TEST_CASE("OSC 66 carries Thai correctly", "[core][osc][sizing]") {
    // The milestone's acceptance case. A Thai cluster is several codepoints
    // that must land in ONE cell, which is exactly why the payload goes through
    // putChar rather than through a second writer here.
    Session session(4, 20);
    feed(session, "\x1b]66;s=2;\xe0\xb8\x81\xe0\xb8\xb2\x1b\\");  // ก + า
    CHECK(session.grid().cellAt(0, 0).ch == U'\u0e01');
    CHECK(session.grid().cellAt(0, 0).attr.scale() == 2);
    CHECK(session.grid().cellAt(0, 1).ch == U'\u0e32');
}

TEST_CASE("an out-of-range OSC 66 field voids the WHOLE sequence", "[core][osc][sizing]") {
    // Clamped would be worse. This is the one OSC that writes text onto the
    // grid, and a clamped scale lays it out at a size the sender did not choose
    // and cannot detect — whereas nothing appearing gets noticed.
    for (const std::string_view bad : {
             "\x1b]66;s=0;X\x1b\\",      // scale below 1
             "\x1b]66;s=8;X\x1b\\",      // scale above 7
             "\x1b]66;w=8;X\x1b\\",      // width above 7
             "\x1b]66;n=16;X\x1b\\",     // numerator above 15
             "\x1b]66;d=16;X\x1b\\",     // denominator above 15
             "\x1b]66;n=3:d=2;X\x1b\\",  // "d must be > n when non-zero"
             "\x1b]66;n=3:d=3;X\x1b\\",
             "\x1b]66;v=3;X\x1b\\",
             "\x1b]66;h=3;X\x1b\\",
             "\x1b]66;s=x;X\x1b\\",  // not a number at all
         }) {
        Session session(4, 20);
        INFO(bad);
        feed(session, bad);
        CHECK(session.grid().cellAt(0, 0).ch == 0);  // nothing written
        CHECK(session.grid().col == 0);
    }
}

TEST_CASE("an unknown OSC 66 key is skipped, not fatal", "[core][osc][sizing]") {
    // Same rule as every other OSC here: the alphabet grows, and refusing over
    // a key we have not heard of would break senders that are entirely correct.
    Session session(4, 20);
    feed(session, "\x1b]66;s=2:Z=9:zz=1;ok\x1b\\");
    CHECK(session.grid().cellAt(0, 0).ch == U'o');
    CHECK(session.grid().cellAt(0, 0).attr.scale() == 2);
}

TEST_CASE("OSC 66 with no text does nothing", "[core][osc][sizing]") {
    Session session(4, 20);
    feed(session, "\x1b]66;s=2;\x1b\\");
    CHECK(session.grid().cellAt(0, 0).ch == 0);
    CHECK(session.grid().col == 0);

    // And a bare OSC 66 with no metadata section at all.
    feed(session, "\x1b]66\x1b\\");
    CHECK(session.grid().col == 0);
}

TEST_CASE("OSC 66 never replies", "[core][osc][sizing]") {
    // It is an instruction, not a query. A reply would inject bytes into the
    // input stream of a program that only asked to draw a word.
    Session session(4, 20);
    std::string replies;
    session.onReply = [&replies](const std::string& text) { replies += text; };
    feed(session, "\x1b]66;s=2:w=3;hello\x1b\\");
    CHECK(replies.empty());
}
