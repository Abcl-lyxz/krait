// libFuzzer target (ADR-0010) for telnet option negotiation.
//
// rules/net.md: "New message handling ships with fuzz seeds." The negotiator is
// the code that parses bytes the far end chose, and it was deliberately built
// with no socket in it so this target can exist at all.
//
// The security claim being tested is the one in the header of
// telnet_negotiation.h: a subnegotiation's parameters reaching the terminal as
// output is a server choosing what appears on the screen.
#include "telnet/telnet_negotiation.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

namespace tn = krait::net::telnet;

struct Pass {
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> reply;
};

// A reply is only ever protocol we generated: IAC-prefixed commands and
// complete subnegotiations. Anything else would mean bytes the far end chose
// are being echoed back into the negotiation.
void checkReplyIsProtocol(const std::vector<std::uint8_t>& reply) {
    for (std::size_t i = 0; i + 1 < reply.size();) {
        assert(reply[i] == tn::kIac && "a reply byte outside an IAC sequence");
        if (reply[i + 1] != tn::kSb) {
            i += 3;  // IAC + command + option
            continue;
        }
        // Walk to the terminating IAC SE, stepping OVER doubled IAC in our own
        // payload — a terminal type containing 0xFF, or a NAWS byte of 255,
        // both emit IAC IAC, and a naive scan would stop on the first half and
        // call a correctly-escaped subnegotiation unterminated.
        std::size_t j = i + 2;
        while (j + 1 < reply.size()) {
            if (reply[j] != tn::kIac) {
                ++j;
                continue;
            }
            if (reply[j + 1] == tn::kIac) {
                j += 2;  // escaped payload byte
                continue;
            }
            break;
        }
        assert(j + 1 < reply.size() && reply[j] == tn::kIac && reply[j + 1] == tn::kSe &&
               "we emitted an unterminated subnegotiation");
        i = j + 2;
    }
}

// Feeds `input` in fixed-size chunks. `offer` sends our opening WILL/DO first,
// which is what puts the option table into the WANTYES states a real client
// spends its first second in — states nothing here reached before, because the
// first version of this target never called start().
Pass feedSplit(std::span<const std::uint8_t> input, std::size_t chunk, bool offer) {
    tn::Negotiator negotiator{tn::TelnetSettings{}};
    Pass pass;
    if (offer) {
        negotiator.start(&pass.reply);
        checkReplyIsProtocol(pass.reply);
    }
    for (std::size_t at = 0; at < input.size(); at += chunk) {
        const std::size_t take = std::min(chunk, input.size() - at);
        negotiator.feed(input.subspan(at, take), &pass.data, &pass.reply);
        checkReplyIsProtocol(pass.reply);
    }
    return pass;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* bytes, std::size_t size) {
    if (size > (1U << 20)) {
        return 0;  // the corpus is not the place to test the allocator
    }
    const std::span<const std::uint8_t> input{bytes, size};
    const std::size_t whole = size == 0 ? 1 : size;

    // TCP does not respect message boundaries, so the SAME bytes split at
    // different points must produce the same output. That is the property that
    // catches a state machine losing track across a split, and comparing the
    // three passes costs nothing — the first version of this target ran all
    // three and threw the results away.
    const Pass byOne = feedSplit(input, 1, false);
    const Pass byThree = feedSplit(input, 3, false);
    const Pass atOnce = feedSplit(input, whole, false);
    assert(byOne.data == byThree.data && byOne.data == atOnce.data &&
           "chunk size changed the terminal output");
    assert(byOne.reply == byThree.reply && byOne.reply == atOnce.reply &&
           "chunk size changed the protocol reply");

    // Same again with our opening offer sent first, so the WANTYES rows of RFC
    // 1143's table are exercised rather than only the NO rows.
    const Pass offered = feedSplit(input, 1, true);
    const Pass offeredAtOnce = feedSplit(input, whole, true);
    assert(offered.data == offeredAtOnce.data && "chunk size changed the output after an offer");
    assert(offered.reply == offeredAtOnce.reply && "chunk size changed the reply after an offer");

    // Whatever the stream did to the negotiator's state, encoding input must
    // still leave every 0xFF doubled — otherwise a server can desynchronise our
    // own writes by driving us into a state we did not anticipate.
    //
    // EVERY RUN of IAC must be even, and that is the whole invariant. The first
    // version asserted "never more than two in a row", which the fuzzer refuted
    // in six executions with `a FF FF b`: two adjacent literal 0xFF bytes
    // correctly become four, because each is escaped as a pair.
    tn::Negotiator negotiator{tn::TelnetSettings{}};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> reply;
    negotiator.feed(input, &data, &reply);

    std::vector<std::uint8_t> encoded;
    negotiator.encodeInput(input, &encoded);
    std::size_t runOfIac = 0;
    for (const std::uint8_t byte : encoded) {
        if (byte == tn::kIac) {
            ++runOfIac;
            continue;
        }
        assert(runOfIac % 2 == 0 && "an odd run of IAC: one was emitted unescaped");
        runOfIac = 0;
    }
    assert(runOfIac % 2 == 0 && "encoded output ends on an unpaired IAC");

    // Resizing must emit nothing before NAWS is agreed — RFC 1143 forbids using
    // an option's effects during negotiation. The width deliberately spans past
    // 255 so a doubled IAC really can appear in the payload, which is what the
    // walker above has to survive.
    std::vector<std::uint8_t> resized;
    negotiator.resize(static_cast<int>(size % 70000), static_cast<int>(size % 300), &resized);
    if (negotiator.us(tn::kNaws) != tn::OptionState::Yes) {
        assert(resized.empty() && "NAWS sent before the option was agreed");
    } else {
        checkReplyIsProtocol(resized);
    }
    return 0;
}
