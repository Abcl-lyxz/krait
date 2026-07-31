// libFuzzer target (ADR-0010) for telnet option negotiation.
//
// rules/net.md: "New message handling ships with fuzz seeds." The negotiator is
// the only new code in T54 that parses bytes chosen by the far end, and it was
// deliberately built with no socket in it so this target can exist at all.
//
// The invariants asserted below are the ones that make the difference between
// a parser bug and a security bug — a subnegotiation's parameters reaching the
// terminal as output is a server choosing what appears on the screen.
#include "telnet/telnet_negotiation.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace {

namespace tn = krait::net::telnet;

// One byte at a time, then in threes, then the whole buffer. TCP does not
// respect message boundaries, so a negotiator that only works on whole PDUs
// works only in a loopback test — and the split points are exactly where a
// state machine forgets what it was doing.
void feedSplit(std::span<const std::uint8_t> input, std::size_t chunk) {
    tn::Negotiator negotiator{tn::TelnetSettings{}};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> reply;
    for (std::size_t at = 0; at < input.size(); at += chunk) {
        const std::size_t take = std::min(chunk, input.size() - at);
        negotiator.feed(input.subspan(at, take), &data, &reply);

        // A reply is only ever protocol: IAC-prefixed commands and
        // subnegotiations we generate ourselves. Anything else would mean bytes
        // the far end chose are being echoed back into the negotiation.
        for (std::size_t i = 0; i + 1 < reply.size();) {
            assert(reply[i] == tn::kIac && "a reply byte outside an IAC sequence");
            if (reply[i + 1] == tn::kSb) {
                // Walk to IAC SE. It must exist: we only ever emit complete
                // subnegotiations.
                std::size_t j = i + 2;
                while (j + 1 < reply.size() && !(reply[j] == tn::kIac && reply[j + 1] == tn::kSe)) {
                    ++j;
                }
                assert(j + 1 < reply.size() && "we emitted an unterminated subnegotiation");
                i = j + 2;
            } else {
                i += 3;  // IAC + command + option
            }
        }
        reply.clear();
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* bytes, std::size_t size) {
    if (size > (1U << 20)) {
        return 0;  // the corpus is not the place to test the allocator
    }
    const std::span<const std::uint8_t> input{bytes, size};

    for (const std::size_t chunk : {std::size_t{1}, std::size_t{3}, size == 0 ? 1 : size}) {
        feedSplit(input, chunk);
    }

    // Whatever the stream did to the negotiator's state, encoding input must
    // still produce a stream in which every 0xFF is doubled — otherwise a
    // server can desynchronise our own writes by driving us into a state we
    // did not anticipate.
    tn::Negotiator negotiator{tn::TelnetSettings{}};
    std::vector<std::uint8_t> data;
    std::vector<std::uint8_t> reply;
    negotiator.feed(input, &data, &reply);

    std::vector<std::uint8_t> encoded;
    negotiator.encodeInput(input, &encoded);
    // EVERY RUN of IAC must be even, and that is the whole invariant. The first
    // version of this asserted "never more than two in a row", which the fuzzer
    // refuted in six executions with `a FF FF b`: two adjacent literal 0xFF
    // bytes correctly become four, because each is escaped as a pair. The
    // assertion was wrong, not the encoder — and an even-run check is the
    // property that actually distinguishes escaped output from a lone IAC that
    // would desynchronise the server's parser for the rest of the session.
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

    // Resizing must never emit anything before NAWS is agreed — RFC 1143
    // forbids using an option's effects during negotiation.
    std::vector<std::uint8_t> resize;
    negotiator.resize(static_cast<int>(size % 5000), static_cast<int>(size % 300), &resize);
    if (negotiator.us(tn::kNaws) != tn::OptionState::Yes) {
        assert(resize.empty() && "NAWS sent before the option was agreed");
    }
    return 0;
}
