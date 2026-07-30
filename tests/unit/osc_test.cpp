#include "core/parser/osc.h"
#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
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
    CHECK(run(session, "\x1b]133;A\x1b\\").empty());
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
