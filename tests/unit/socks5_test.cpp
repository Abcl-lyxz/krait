// T59: the SOCKS5 handshake for dynamic forwards.
//
// The bytes come from whatever someone points at the port — a browser, curl, a
// scanner — so this is a hostile-input parser and the interesting cases are the
// malformed ones. Constants and layouts were verified against RFC 1928 before
// the code was written; where the RFC is silent (truncation, zero-length names,
// buffer limits) the choice is ours and is asserted here so it cannot drift.

#include "ssh/socks5.h"
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace s5 = krait::net::socks5;

namespace {

using Bytes = std::vector<std::uint8_t>;

// The reply to a completed CONNECT: VER REP RSV ATYP=IPv4 then six zero bytes.
Bytes replyWith(std::uint8_t code) {
    return {0x05, code, 0x00, 0x01, 0, 0, 0, 0, 0, 0};
}

// A function, not a namespace-scope constant: a std::vector with static
// storage duration can throw during initialisation, where nothing can catch it.
Bytes greeting() {
    return {0x05, 0x01, 0x00};  // one method: no authentication
}

}  // namespace

TEST_CASE("the greeting is answered with no-authentication", "[socks5]") {
    s5::Handshake handshake;
    Bytes reply;
    CHECK(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    CHECK(reply == Bytes{0x05, 0x00});
}

TEST_CASE("a client offering no acceptable method is refused", "[socks5]") {
    // 0xFF is "NO ACCEPTABLE METHODS". RFC 1928 puts the MUST-close on the
    // CLIENT, not on us — we close anyway, but the reply is what is required.
    s5::Handshake handshake;
    Bytes reply;
    const Bytes onlyGssapi{0x05, 0x01, 0x01};
    CHECK(handshake.feed(onlyGssapi, &reply) == s5::Phase::Failed);
    CHECK(reply == Bytes{0x05, 0xFF});
}

TEST_CASE("a CONNECT to a domain name yields host and port", "[socks5]") {
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);

    // VER CMD RSV ATYP LEN "example.com" PORT(443, big-endian)
    Bytes request{0x05, 0x01, 0x00, 0x03, 11};
    for (const char ch : std::string("example.com")) {
        request.push_back(static_cast<std::uint8_t>(ch));
    }
    request.push_back(0x01);
    request.push_back(0xBB);

    reply.clear();
    REQUIRE(handshake.feed(request, &reply) == s5::Phase::Ready);
    CHECK(handshake.host() == "example.com");
    CHECK(handshake.port() == 443);
    // NOTHING is sent yet: the reply has to report whether the connection
    // succeeded, and nothing has been attempted.
    CHECK(reply.empty());

    handshake.finish(true, &reply);
    CHECK(handshake.phase() == s5::Phase::Done);
    CHECK(reply == replyWith(0x00));
}

TEST_CASE("the domain form is length-prefixed with no NUL", "[socks5]") {
    // RFC 1928 section 5: "there is no terminating NUL octet". On-wire size is
    // exactly 1 + len, and reading len + 1 is the off-by-one that walks off the
    // end of the buffer.
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);

    // A name followed immediately by the port, with no NUL between them.
    const Bytes request{0x05, 0x01, 0x00, 0x03, 2, 'h', 'i', 0x00, 0x50};
    reply.clear();
    REQUIRE(handshake.feed(request, &reply) == s5::Phase::Ready);
    CHECK(handshake.host() == "hi");
    CHECK(handshake.port() == 80);
}

TEST_CASE("IPv4 and IPv6 addresses are formatted for resolution", "[socks5]") {
    {
        s5::Handshake handshake;
        Bytes reply;
        REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
        const Bytes request{0x05, 0x01, 0x00, 0x01, 10, 0, 0, 7, 0x1F, 0x90};
        REQUIRE(handshake.feed(request, &reply) == s5::Phase::Ready);
        CHECK(handshake.host() == "10.0.0.7");
        CHECK(handshake.port() == 8080);
    }
    {
        s5::Handshake handshake;
        Bytes reply;
        REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
        Bytes request{0x05, 0x01, 0x00, 0x04};
        for (int i = 0; i < 15; ++i) {
            request.push_back(0x00);
        }
        request.push_back(0x01);  // ::1
        request.push_back(0x00);
        request.push_back(0x16);
        REQUIRE(handshake.feed(request, &reply) == s5::Phase::Ready);
        CHECK(handshake.host() == "0000:0000:0000:0000:0000:0000:0000:0001");
        CHECK(handshake.port() == 22);
    }
}

TEST_CASE("greeting and request coalesced into one read still parse", "[socks5]") {
    // RFC 1928 says NOTHING about framing, and curl pipelines these into one
    // segment. A parser that handled one message per read would misparse
    // exactly the clients most likely to be used.
    s5::Handshake handshake;
    Bytes both = greeting();
    const Bytes request{0x05, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x00, 0x50};
    both.insert(both.end(), request.begin(), request.end());

    Bytes reply;
    REQUIRE(handshake.feed(both, &reply) == s5::Phase::Ready);
    CHECK(handshake.host() == "127.0.0.1");
    CHECK(handshake.port() == 80);
    CHECK(reply == Bytes{0x05, 0x00});  // only the method selection so far
}

TEST_CASE("a request arriving one byte at a time still parses", "[socks5]") {
    // The other half of the same problem: TCP may deliver anything in any
    // number of pieces, and every partial state has to be resumable.
    s5::Handshake handshake;
    Bytes all = greeting();
    const Bytes request{0x05, 0x01, 0x00, 0x03, 3, 'a', 'b', 'c', 0x1F, 0x90};
    all.insert(all.end(), request.begin(), request.end());

    Bytes reply;
    s5::Phase phase = s5::Phase::Greeting;
    for (const std::uint8_t byte : all) {
        phase = handshake.feed(Bytes{byte}, &reply);
    }
    CHECK(phase == s5::Phase::Ready);
    CHECK(handshake.host() == "abc");
    CHECK(handshake.port() == 8080);
}

TEST_CASE("BIND and UDP ASSOCIATE are refused with the right code", "[socks5]") {
    // 0x07 is "Command not supported". Refusing with the specific code lets a
    // client report it instead of timing out.
    for (const std::uint8_t command : {std::uint8_t{0x02}, std::uint8_t{0x03}}) {
        s5::Handshake handshake;
        Bytes reply;
        REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
        reply.clear();
        const Bytes request{0x05, command, 0x00, 0x01, 127, 0, 0, 1, 0x00, 0x50};
        CHECK(handshake.feed(request, &reply) == s5::Phase::Failed);
        CHECK(reply == replyWith(0x07));
    }
}

TEST_CASE("an unknown address type is refused", "[socks5]") {
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    reply.clear();
    const Bytes request{0x05, 0x01, 0x00, 0x09, 1, 2, 3, 4, 0x00, 0x50};
    CHECK(handshake.feed(request, &reply) == s5::Phase::Failed);
    CHECK(reply == replyWith(0x08));  // "Address type not supported"
}

TEST_CASE("a zero-length domain name is refused rather than resolved", "[socks5]") {
    // The RFC does not cover len == 0. Ours: refuse it, because resolving an
    // empty host is a request to connect somewhere nobody named.
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    reply.clear();
    const Bytes request{0x05, 0x01, 0x00, 0x03, 0, 0x00, 0x50};
    CHECK(handshake.feed(request, &reply) == s5::Phase::Failed);
    CHECK(reply == replyWith(0x08));
}

TEST_CASE("a wrong version is refused at both stages", "[socks5]") {
    {
        // No reply is defined for a bad greeting version, so nothing is sent.
        s5::Handshake handshake;
        Bytes reply;
        CHECK(handshake.feed(Bytes{0x04, 0x01, 0x00}, &reply) == s5::Phase::Failed);
        CHECK(reply.empty());
    }
    {
        s5::Handshake handshake;
        Bytes reply;
        REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
        reply.clear();
        const Bytes request{0x04, 0x01, 0x00, 0x01, 127, 0, 0, 1, 0x00, 0x50};
        CHECK(handshake.feed(request, &reply) == s5::Phase::Failed);
        CHECK(reply == replyWith(0x01));  // general failure
    }
}

TEST_CASE("a truncated request never completes and never overruns", "[socks5]") {
    // The RFC says nothing about truncation. Ours: stay in Request, emit
    // nothing, and — crucially — do not read past what arrived. A length byte
    // claiming 255 with two bytes present is the overrun this prevents.
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    reply.clear();
    CHECK(handshake.feed(Bytes{0x05, 0x01, 0x00, 0x03, 255, 'a', 'b'}, &reply) ==
          s5::Phase::Request);
    CHECK(reply.empty());
    CHECK(handshake.host().empty());
}

TEST_CASE("a client that floods without completing is dropped", "[socks5]") {
    // The buffer is bounded because RFC 1928 imposes no limit anywhere. A
    // greeting plus a 255-byte name is under 300 bytes; anything past the cap
    // is not a SOCKS client, and without the cap it is a remote allocation
    // primitive on a port the user opened for convenience.
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    const Bytes flood(4096, 0x00);
    CHECK(handshake.feed(flood, &reply) == s5::Phase::Failed);
}

TEST_CASE("a refused connection is reported, not left hanging", "[socks5]") {
    // 0x05 is "Connection refused". Without it the client waits for a reply
    // that never comes, which reads as a hang rather than a failure.
    s5::Handshake handshake;
    Bytes reply;
    REQUIRE(handshake.feed(greeting(), &reply) == s5::Phase::Request);
    const Bytes request{0x05, 0x01, 0x00, 0x01, 10, 0, 0, 1, 0x00, 0x16};
    REQUIRE(handshake.feed(request, &reply) == s5::Phase::Ready);

    reply.clear();
    handshake.finish(false, &reply);
    CHECK(handshake.phase() == s5::Phase::Failed);
    CHECK(reply == replyWith(0x05));
}

TEST_CASE("finish before the request is parsed does nothing", "[socks5]") {
    s5::Handshake handshake;
    Bytes reply;
    handshake.finish(true, &reply);
    CHECK(reply.empty());
    CHECK(handshake.phase() == s5::Phase::Greeting);
}
