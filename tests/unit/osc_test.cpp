#include "core/parser/osc.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

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
    // Codes we do not implement stay silent rather than half-handled.
    CHECK(run(session, "\x1b]4;1;rgb:00/00/00\x1b\\").empty());
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
