// T54: telnet option negotiation. The negotiator has no socket in it, so every
// case here is a byte string in and a byte string out — including the ones a
// hostile server would send, which is the reason it was separated from the
// backend at all.
//
// Constants and rules cited here are in docs/research/t54-telnet-findings.md,
// checked against the RFCs before the code was written.

#include "telnet/telnet_negotiation.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace tn = krait::net::telnet;

namespace {

using Bytes = std::vector<std::uint8_t>;

struct Session {
    tn::Negotiator negotiator{tn::TelnetSettings{}};
    Bytes data;
    Bytes reply;

    void feed(const Bytes& bytes) {
        data.clear();
        reply.clear();
        negotiator.feed(bytes, &data, &reply);
    }

    std::string text() const { return {data.begin(), data.end()}; }
};

// Readable failure output beats a wall of decimal.
std::string hex(const Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
        out.push_back(' ');
    }
    return out;
}

}  // namespace

TEST_CASE("data passes through and doubled IAC becomes one byte", "[telnet]") {
    Session session;
    session.feed({'h', 'i', tn::kIac, tn::kIac, '!'});
    REQUIRE(session.data.size() == 4);
    CHECK(session.data[2] == 0xFF);
    CHECK(session.reply.empty());
}

TEST_CASE("an unsupported option is refused, never ignored", "[telnet]") {
    // RFC 1123 makes this a MUST: "A host MUST refuse (i.e, reply WONT/DONT to
    // a DO/WILL) an unsupported option." Silence would leave the server waiting
    // and, for some servers, retrying forever.
    Session session;
    session.feed({tn::kIac, tn::kWill, 99});
    CHECK(session.reply == Bytes{tn::kIac, tn::kDont, 99});

    session.feed({tn::kIac, tn::kDo, 99});
    CHECK(session.reply == Bytes{tn::kIac, tn::kWont, 99});
    CHECK(session.data.empty());
}

TEST_CASE("NEW-ENVIRON and AUTHENTICATION are refused by policy", "[telnet]") {
    // Not because the RFCs say so — they do not. NEW-ENVIRON would ship the
    // environment to the far end, and RFC 2941 authentication negotiates its
    // own strength in the clear.
    Session session;
    session.feed({tn::kIac, tn::kDo, tn::kNewEnviron});
    CHECK(session.reply == Bytes{tn::kIac, tn::kWont, tn::kNewEnviron});

    session.feed({tn::kIac, tn::kDo, tn::kAuthentication});
    CHECK(session.reply == Bytes{tn::kIac, tn::kWont, tn::kAuthentication});
}

TEST_CASE("we never agree to echo", "[telnet]") {
    // RFC 857: both ends echoing "loops back and forth indefinitely". A client
    // that echoes also echoes the password the server just asked it not to.
    Session session;
    session.feed({tn::kIac, tn::kDo, tn::kEcho});
    CHECK(session.reply == Bytes{tn::kIac, tn::kWont, tn::kEcho});
    CHECK(session.negotiator.us(tn::kEcho) == tn::OptionState::No);
}

TEST_CASE("the server taking over echo is visible", "[telnet]") {
    // This is how a password prompt is detected, so it has to be observable.
    Session session;
    session.feed({tn::kIac, tn::kWill, tn::kEcho});
    CHECK(session.negotiator.serverEchoes());
    CHECK(session.reply == Bytes{tn::kIac, tn::kDo, tn::kEcho});
}

TEST_CASE("an option already agreed is not acknowledged again", "[telnet]") {
    // RFC 854 calls this non-response "essential to prevent endless loops in
    // the negotiation". A server that repeats WILL must get silence, not a
    // second DO — two implementations both being polite is the loop.
    Session session;
    session.feed({tn::kIac, tn::kWill, tn::kSuppressGoAhead});
    REQUIRE(session.reply == Bytes{tn::kIac, tn::kDo, tn::kSuppressGoAhead});

    session.feed({tn::kIac, tn::kWill, tn::kSuppressGoAhead});
    INFO("second WILL produced: " << hex(session.reply));
    CHECK(session.reply.empty());

    // And the same for a refusal we have already made.
    session.feed({tn::kIac, tn::kWill, 99});
    REQUIRE(session.reply == Bytes{tn::kIac, tn::kDont, 99});
    session.feed({tn::kIac, tn::kWont, 99});
    CHECK(session.reply.empty());
}

TEST_CASE("NAWS is sent on agreement and on resize, big-endian", "[telnet]") {
    Session session;
    session.negotiator.resize(80, 24, &session.reply);
    // Not agreed yet: RFC 1143 forbids using an option's effects before it is
    // enabled, so nothing goes out.
    CHECK(session.reply.empty());

    session.feed({tn::kIac, tn::kDo, tn::kNaws});
    // WILL NAWS, then the size — RFC 1073's example for 80x24 is exactly this.
    CHECK(session.reply == Bytes{tn::kIac, tn::kWill, tn::kNaws, tn::kIac, tn::kSb, tn::kNaws, 0,
                                 80, 0, 24, tn::kIac, tn::kSe});

    session.reply.clear();
    session.negotiator.resize(132, 43, &session.reply);
    CHECK(session.reply == Bytes{tn::kIac, tn::kSb, tn::kNaws, 0, 132, 0, 43, tn::kIac, tn::kSe});
}

TEST_CASE("a NAWS byte of 255 is doubled", "[telnet]") {
    // RFC 1073: "any occurrence of 255 in the subnegotiation must be doubled".
    // Needs a 255-column terminal to hit, which is why it is the rule that gets
    // dropped — and dropping it desynchronises the server's parser for good.
    Session session;
    session.feed({tn::kIac, tn::kDo, tn::kNaws});
    session.reply.clear();
    session.negotiator.resize(255, 24, &session.reply);
    CHECK(session.reply ==
          Bytes{tn::kIac, tn::kSb, tn::kNaws, 0, 0xFF, 0xFF, 0, 24, tn::kIac, tn::kSe});
}

TEST_CASE("terminal type is answered only after we agreed to it", "[telnet]") {
    Session session;
    // Unsolicited SB before any agreement gets nothing: answering would let a
    // server drive us with an option it merely claimed.
    session.feed({tn::kIac, tn::kSb, tn::kTerminalType, tn::kTerminalTypeSend, tn::kIac, tn::kSe});
    CHECK(session.reply.empty());

    session.feed({tn::kIac, tn::kDo, tn::kTerminalType});
    REQUIRE(session.reply == Bytes{tn::kIac, tn::kWill, tn::kTerminalType});

    session.feed({tn::kIac, tn::kSb, tn::kTerminalType, tn::kTerminalTypeSend, tn::kIac, tn::kSe});
    Bytes expected{tn::kIac, tn::kSb, tn::kTerminalType, tn::kTerminalTypeIs};
    for (const char ch : std::string("xterm-256color")) {
        expected.push_back(static_cast<std::uint8_t>(ch));
    }
    expected.push_back(tn::kIac);
    expected.push_back(tn::kSe);
    CHECK(session.reply == expected);
}

TEST_CASE("only IAC SE ends a subnegotiation", "[telnet]") {
    // RFC 855: the terminator is the STRING IAC SE. A bare 240 in parameter
    // data is data — a parser that stops on it lets a server end its own
    // subnegotiation early and have the rest land in the terminal as output.
    Session session;
    session.feed({tn::kIac, tn::kDo, tn::kTerminalType});
    session.feed(
        {tn::kIac, tn::kSb, tn::kTerminalType, tn::kSe, tn::kTerminalTypeSend, tn::kIac, tn::kSe});
    // The bare SE was consumed as parameter data, so the first byte is not
    // SEND and there is no answer — and crucially nothing reached the terminal.
    CHECK(session.data.empty());
    CHECK(session.reply.empty());
}

TEST_CASE("an oversized subnegotiation is dropped, not truncated into output", "[telnet]") {
    // The RFCs impose NO bound on a subnegotiation, so this cap is ours. The
    // failure mode being prevented is not the allocation: it is a parser that
    // gives up mid-subnegotiation and starts treating attacker-chosen
    // parameter bytes as terminal output.
    Session session;
    Bytes flood{tn::kIac, tn::kSb, tn::kTerminalType};
    flood.insert(flood.end(), tn::kMaxSubnegotiation * 4, 'A');
    flood.push_back(tn::kIac);
    flood.push_back(tn::kSe);

    session.feed(flood);
    CHECK(session.data.empty());  // not one byte of it reached the terminal
    CHECK(session.reply.empty());
    CHECK(session.negotiator.droppedSubnegotiations() == 1);

    // And the stream is still usable afterwards, which is the other half:
    // dropping a subnegotiation must not desynchronise everything after it.
    session.feed({'o', 'k'});
    CHECK(session.text() == "ok");
}

TEST_CASE("an unterminated subnegotiation never emits output", "[telnet]") {
    // Unspecified by RFC 855. Ours: hold, bounded, and emit nothing. The
    // alternative — flushing the parameters as data when the stream ends — is
    // a server choosing what appears on the screen.
    Session session;
    session.feed({tn::kIac, tn::kSb, tn::kTerminalType, 'n', 'e', 'v', 'e', 'r'});
    CHECK(session.data.empty());
    CHECK(session.reply.empty());
}

TEST_CASE("an unknown command byte is consumed whole", "[telnet]") {
    // RFC 1123: "A host MUST be able to receive and ignore any Telnet control
    // functions that it does not support." Both bytes go, and the byte after
    // must still be data — dropping only the IAC would put the command byte on
    // screen.
    Session session;
    session.feed({'a', tn::kIac, tn::kNop, 'b', tn::kIac, tn::kGoAhead, 'c'});
    CHECK(session.text() == "abc");
}

TEST_CASE("negotiation split across reads is not lost", "[telnet]") {
    // TCP does not respect message boundaries, so every state has to survive
    // arriving one byte at a time. This is the case a table-driven parser
    // passes and an index-into-the-buffer one fails.
    Session session;
    Bytes total;
    const Bytes stream{tn::kIac, tn::kWill, tn::kSuppressGoAhead};
    for (const std::uint8_t byte : stream) {
        session.feed({byte});
        total.insert(total.end(), session.reply.begin(), session.reply.end());
    }
    CHECK(total == Bytes{tn::kIac, tn::kDo, tn::kSuppressGoAhead});
    CHECK(session.negotiator.him(tn::kSuppressGoAhead) == tn::OptionState::Yes);
}

TEST_CASE("input is escaped and CR gets its NVT companion", "[telnet]") {
    Session session;
    Bytes out;
    const Bytes input{'l', 's', 0xFF, '\r'};
    session.negotiator.encodeInput(input, &out);
    // 0xFF doubled (RFC 854), and CR NUL because "the CR character must be
    // avoided in other contexts" — a bare CR is the one thing the spec says not
    // to send, and Enter produces one every time.
    CHECK(out == Bytes{'l', 's', 0xFF, 0xFF, '\r', 0x00});
}

TEST_CASE("BINARY drops the CR rule but keeps IAC doubling", "[telnet]") {
    // RFC 1123: "there is no end-of-line convention ... in binary mode". RFC
    // 856 keeps the doubling, which is the half that gets dropped with it.
    Session session;
    session.feed({tn::kIac, tn::kDo, tn::kBinary});
    REQUIRE(session.negotiator.us(tn::kBinary) == tn::OptionState::Yes);

    Bytes out;
    const Bytes input{'x', '\r', 0xFF};
    session.negotiator.encodeInput(input, &out);
    CHECK(out == Bytes{'x', '\r', 0xFF, 0xFF});
}

TEST_CASE("the opening offer is the four options we actually support", "[telnet]") {
    Session session;
    session.negotiator.start(&session.reply);
    CHECK(session.reply == Bytes{tn::kIac, tn::kWill, tn::kTerminalType, tn::kIac, tn::kWill,
                                 tn::kNaws, tn::kIac, tn::kDo, tn::kSuppressGoAhead, tn::kIac,
                                 tn::kDo, tn::kEcho});
}

TEST_CASE("a server answering our offer completes the handshake", "[telnet]") {
    // The WANTYES half of RFC 1143's table: our own WILL is outstanding, and
    // the DO that answers it must land as YES without emitting a second WILL.
    Session session;
    session.negotiator.start(&session.reply);
    session.feed({tn::kIac, tn::kDo, tn::kTerminalType});
    CHECK(session.negotiator.us(tn::kTerminalType) == tn::OptionState::Yes);
    INFO("answering our own WILL produced: " << hex(session.reply));
    CHECK(session.reply.empty());

    // A refusal of our offer settles at No, also silently.
    session.feed({tn::kIac, tn::kDont, tn::kNaws});
    CHECK(session.negotiator.us(tn::kNaws) == tn::OptionState::No);
    CHECK(session.reply.empty());
}
