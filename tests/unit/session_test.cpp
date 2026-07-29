#include "core/terminal/session.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using krait::core::vt::Session;

namespace {

void feedString(Session& s, const std::string& bytes) {
    s.feed({reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()});
}

}  // namespace

TEST_CASE("session: bytes flow through parser into the grid", "[session]") {
    Session s(24, 80);
    feedString(s, "hi\r\nthere");
    CHECK(s.grid().cellAt(0, 0).ch == U'h');
    CHECK(s.grid().cellAt(0, 1).ch == U'i');
    CHECK(s.grid().cellAt(1, 0).ch == U't');
    CHECK(s.grid().row == 1);
    CHECK(s.grid().col == 5);
}

TEST_CASE("session: DA1 fires onReply exactly once with the table reply", "[session]") {
    Session s(24, 80);
    std::vector<std::string> replies;
    s.onReply = [&replies](const std::string& r) { replies.push_back(r); };
    feedString(s, "\x1b[c");
    REQUIRE(replies.size() == 1);
    CHECK(replies[0] == "\x1b[?1;2c");
}

TEST_CASE("session: onReply may safely queue more input mid-feed", "[session]") {
    // Pins the re-entrancy contract the ConPTY wiring relies on: the reply
    // callback runs during feed(); collecting/queuing there must not
    // corrupt the parse in progress.
    Session s(24, 80);
    std::string queued;
    s.onReply = [&queued](const std::string& r) { queued += r; };
    feedString(s, "ab\x1b[6ncd");  // DSR 6 mid-stream
    CHECK(queued == "\x1b[1;3R");  // CPR after "ab"
    CHECK(s.grid().cellAt(0, 2).ch == U'c');
    CHECK(s.grid().cellAt(0, 3).ch == U'd');
    CHECK(s.grid().col == 4);
}

TEST_CASE("session: no reply sink installed is safe", "[session]") {
    Session s(24, 80);
    feedString(s, "\x1b[c\x1b[5n");  // both would reply; nothing to call
    CHECK(s.grid().row == 0);        // and nothing crashed
}
