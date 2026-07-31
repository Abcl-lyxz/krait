// T59: parsing port-forward specs.
//
// This is a parser whose failure mode is a hole. A spec that is misread does
// not error — it opens a listener somewhere other than intended, and nothing
// on screen says so. So the cases that matter most here are the REFUSALS.

#include "ssh/forwards.h"
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

using krait::net::Forward;
using krait::net::ForwardKind;
using krait::net::parseForward;
using krait::net::parseForwards;

TEST_CASE("the three-field local form", "[forwards]") {
    Forward forward;
    REQUIRE(parseForward(ForwardKind::Local, "8080:internal:80", &forward));
    CHECK(forward.bindPort == 8080);
    CHECK(forward.bindAddress.empty());  // loopback
    CHECK(forward.destHost == "internal");
    CHECK(forward.destPort == 80);
}

TEST_CASE("an explicit bind address is kept", "[forwards]") {
    // The four-field form is how someone deliberately exposes a tunnel to the
    // network. It has to be possible AND it has to be explicit — which is why
    // the three-field form leaves the address empty rather than defaulting to
    // 0.0.0.0. A forward the user thought was private is the whole risk here.
    Forward forward;
    REQUIRE(parseForward(ForwardKind::Local, "0.0.0.0:8080:internal:80", &forward));
    CHECK(forward.bindAddress == "0.0.0.0");
    CHECK(forward.bindPort == 8080);
}

TEST_CASE("IPv6 literals survive the split", "[forwards]") {
    // ssh_config brackets them precisely because the separator is also part of
    // the address. A splitter that does not know about brackets turns this into
    // a pile of empty fields and then rejects a perfectly good spec.
    Forward forward;
    REQUIRE(parseForward(ForwardKind::Local, "[::1]:8080:[fe80::2]:80", &forward));
    CHECK(forward.bindAddress == "::1");
    CHECK(forward.bindPort == 8080);
    CHECK(forward.destHost == "fe80::2");
    CHECK(forward.destPort == 80);
}

TEST_CASE("dynamic forwards take a port and nothing else", "[forwards]") {
    Forward forward;
    REQUIRE(parseForward(ForwardKind::Dynamic, "1080", &forward));
    CHECK(forward.bindPort == 1080);
    CHECK(forward.destHost.empty());  // learned per connection from SOCKS

    REQUIRE(parseForward(ForwardKind::Dynamic, "127.0.0.1:1080", &forward));
    CHECK(forward.bindAddress == "127.0.0.1");
    CHECK(forward.bindPort == 1080);

    // A destination is meaningless here, and accepting it would mean the user
    // believes traffic goes somewhere it does not.
    CHECK_FALSE(parseForward(ForwardKind::Dynamic, "1080:internal:80", &forward));
}

TEST_CASE("a port that is not entirely a number is refused", "[forwards]") {
    // "80x" parsing as 80 would forward a port nobody wrote. A trailing
    // character is far more likely to be a typo in the host than a port the
    // user meant, so the whole field has to be the number.
    Forward forward;
    CHECK_FALSE(parseForward(ForwardKind::Local, "80x:internal:80", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "8080:internal:80y", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, ":internal:80", &forward));
}

TEST_CASE("ports outside the wire range are refused", "[forwards]") {
    Forward forward;
    CHECK_FALSE(parseForward(ForwardKind::Local, "0:internal:80", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "65536:internal:80", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "-1:internal:80", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "8080:internal:0", &forward));
}

TEST_CASE("a spec with the wrong number of fields is refused", "[forwards]") {
    // The dangerous misreading: "8080:internal" has no destination port, and a
    // parser that filled one in would tunnel to a service nobody named.
    Forward forward;
    CHECK_FALSE(parseForward(ForwardKind::Local, "8080", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "8080:internal", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "a:8080:internal:80:extra", &forward));
    CHECK_FALSE(parseForward(ForwardKind::Local, "", &forward));
    // An empty destination host is nowhere to send it.
    CHECK_FALSE(parseForward(ForwardKind::Local, "8080::80", &forward));
}

TEST_CASE("a list keeps the good entries and names the bad ones", "[forwards]") {
    // One typo must not silently disable the other four tunnels — and a tunnel
    // that did not open is indistinguishable from a server that is down, so
    // the rejects are NAMED rather than counted.
    std::vector<std::string> rejected;
    const std::vector<Forward> forwards = parseForwards(
        "L 8080:internal:80, D 1080, X nonsense, R 9000:localhost:22, L bad", &rejected);

    REQUIRE(forwards.size() == 3);
    CHECK(forwards[0].kind == ForwardKind::Local);
    CHECK(forwards[1].kind == ForwardKind::Dynamic);
    CHECK(forwards[2].kind == ForwardKind::Remote);
    CHECK(forwards[2].destPort == 22);

    REQUIRE(rejected.size() == 2);
    CHECK(rejected[0] == "X nonsense");
    CHECK(rejected[1] == "L bad");
}

TEST_CASE("lowercase letters and stray whitespace are tolerated", "[forwards]") {
    std::vector<std::string> rejected;
    const std::vector<Forward> forwards =
        parseForwards("  l 8080:internal:80 ,, d 1080 ,", &rejected);
    CHECK(forwards.size() == 2);
    CHECK(rejected.empty());
}

TEST_CASE("describe says which way a tunnel points", "[forwards]") {
    // It goes in the tunnel pane, and a pane that cannot tell an L from an R is
    // a pane that will get someone's staging database exposed.
    Forward local;
    REQUIRE(parseForward(ForwardKind::Local, "8080:internal:80", &local));
    CHECK(local.describe() == "L 8080 -> internal:80");

    Forward remote;
    REQUIRE(parseForward(ForwardKind::Remote, "0.0.0.0:9000:localhost:22", &remote));
    CHECK(remote.describe() == "R 0.0.0.0:9000 -> localhost:22");

    Forward socks;
    REQUIRE(parseForward(ForwardKind::Dynamic, "1080", &socks));
    CHECK(socks.describe() == "D 1080 -> SOCKS");
}
