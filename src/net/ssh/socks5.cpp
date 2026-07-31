#include "socks5.h"

#include <algorithm>

namespace krait::net::socks5 {
namespace {

// RFC 1928. Every constant here is quoted from the RFC rather than remembered.
constexpr std::uint8_t kVersion = 0x05;

// Section 3, method identifiers.
constexpr std::uint8_t kNoAuth = 0x00;        // "NO AUTHENTICATION REQUIRED"
constexpr std::uint8_t kNoAcceptable = 0xFF;  // "NO ACCEPTABLE METHODS"

// Section 4, CMD.
constexpr std::uint8_t kConnect = 0x01;  // BIND is 0x02, UDP ASSOCIATE 0x03

// Section 5, ATYP.
constexpr std::uint8_t kIPv4 = 0x01;    // 4 octets
constexpr std::uint8_t kDomain = 0x03;  // length-prefixed, NO terminating NUL
constexpr std::uint8_t kIPv6 = 0x04;    // 16 octets

// Section 6, REP.
constexpr std::uint8_t kSucceeded = 0x00;
constexpr std::uint8_t kGeneralFailure = 0x01;
constexpr std::uint8_t kConnectionRefused = 0x05;
constexpr std::uint8_t kCommandNotSupported = 0x07;
constexpr std::uint8_t kAddressNotSupported = 0x08;

// Ours, not the RFC's: RFC 1928 states no maximum for a domain name beyond
// what the single length octet implies, and says nothing at all about
// malformed or truncated requests. A greeting plus a request with a 255-byte
// name is under 300 bytes, so anything past this is not a SOCKS client and the
// buffer must not grow for it.
constexpr std::size_t kMaxBuffered = 1024;

// The reply's BND.ADDR/BND.PORT. RFC 1928 says these carry "the port number
// that the server assigned to connect to the target host" and its address —
// which for an SSH direct-tcpip channel does not exist in any form we can
// report. All-zero with ATYP=IPv4 is what every -D implementation sends and
// what clients ignore; it is CONVENTION, not something the RFC blesses, and
// saying so here is cheaper than someone later hunting for the citation.
//
// The reply's ATYP is NOT required to match the request's — nothing in section
// 6 ties them together — so a domain-name request still gets an IPv4 zero.
void appendReply(std::vector<std::uint8_t>* reply, std::uint8_t code) {
    reply->push_back(kVersion);
    reply->push_back(code);
    reply->push_back(0x00);  // RSV
    reply->push_back(kIPv4);
    for (int i = 0; i < 4; ++i) {
        reply->push_back(0x00);  // BND.ADDR
    }
    reply->push_back(0x00);  // BND.PORT, network octet order
    reply->push_back(0x00);
}

std::string formatIPv4(const std::uint8_t* octets) {
    std::string out;
    for (int i = 0; i < 4; ++i) {
        if (i > 0) {
            out += '.';
        }
        out += std::to_string(octets[i]);
    }
    return out;
}

std::string formatIPv6(const std::uint8_t* octets) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    for (int group = 0; group < 8; ++group) {
        if (group > 0) {
            out += ':';
        }
        // std::size_t on the index, not int: `group * 2` computed in int and
        // then widened to a pointer offset is the habit that overflows the day
        // someone reuses this for something longer than 16 bytes.
        const std::size_t at = static_cast<std::size_t>(group) * 2;
        const std::uint8_t high = octets[at];
        const std::uint8_t low = octets[at + 1];
        out += kHex[high >> 4];
        out += kHex[high & 0x0F];
        out += kHex[low >> 4];
        out += kHex[low & 0x0F];
    }
    // Deliberately NOT compressed to the :: form. This string is handed to
    // libssh to resolve, an expanded address is unambiguous, and producing a
    // shortened one correctly is a second parser nobody asked for.
    return out;
}

}  // namespace

Phase Handshake::fail(std::vector<std::uint8_t>* reply, std::uint8_t code) {
    appendReply(reply, code);
    m_phase = Phase::Failed;
    return m_phase;
}

Phase Handshake::feed(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* reply) {
    if (m_phase == Phase::Failed || m_phase == Phase::Done || m_phase == Phase::Ready) {
        return m_phase;
    }
    if (m_buffer.size() + bytes.size() > kMaxBuffered) {
        // Not a SOCKS client. Nothing is echoed and nothing grows.
        m_phase = Phase::Failed;
        return m_phase;
    }
    m_buffer.insert(m_buffer.end(), bytes.begin(), bytes.end());

    // A loop, not an if: RFC 1928 says NOTHING about framing, and a client is
    // free to pipeline its greeting and request into one segment — curl does.
    // A parser that handled one message per read would misparse exactly those
    // clients.
    while (true) {
        if (m_phase == Phase::Greeting) {
            // VER NMETHODS METHODS...
            if (m_buffer.size() < 2) {
                return m_phase;
            }
            if (m_buffer[0] != kVersion) {
                // No reply is defined for a bad greeting version — section 3
                // does not cover it — so the connection simply ends.
                m_phase = Phase::Failed;
                return m_phase;
            }
            const std::size_t methods = m_buffer[1];
            if (m_buffer.size() < 2 + methods) {
                return m_phase;
            }
            const auto first = m_buffer.begin() + 2;
            const auto last = first + static_cast<std::ptrdiff_t>(methods);
            const bool offersNoAuth = std::find(first, last, kNoAuth) != last;
            m_buffer.erase(m_buffer.begin(), last);

            // We offer only "no authentication", which section 3 arguably
            // forbids — "Compliant implementations MUST support GSSAPI" — and
            // which every -D implementation ships anyway. A deliberate
            // deviation, not an oversight: the tunnel is already authenticated
            // by SSH, and a second credential on a loopback socket protects
            // nothing while giving the user something else to configure.
            reply->push_back(kVersion);
            reply->push_back(offersNoAuth ? kNoAuth : kNoAcceptable);
            if (!offersNoAuth) {
                m_phase = Phase::Failed;
                return m_phase;
            }
            m_phase = Phase::Request;
            continue;
        }

        // VER CMD RSV ATYP DST.ADDR DST.PORT
        if (m_buffer.size() < 4) {
            return m_phase;
        }
        if (m_buffer[0] != kVersion) {
            return fail(reply, kGeneralFailure);
        }
        if (m_buffer[1] != kConnect) {
            // BIND and UDP ASSOCIATE. Refused with the code that says exactly
            // that, so a client can report it rather than time out.
            return fail(reply, kCommandNotSupported);
        }
        m_addressType = m_buffer[3];

        std::size_t addressBytes = 0;
        std::size_t addressAt = 4;
        switch (m_addressType) {
        case kIPv4:
            addressBytes = 4;
            break;
        case kIPv6:
            addressBytes = 16;
            break;
        case kDomain:
            if (m_buffer.size() < 5) {
                return m_phase;  // the length octet has not arrived
            }
            addressBytes = m_buffer[4];
            addressAt = 5;
            if (addressBytes == 0) {
                // Zero-length is not covered by the RFC. Refused rather than
                // resolved as an empty host.
                return fail(reply, kAddressNotSupported);
            }
            break;
        default:
            return fail(reply, kAddressNotSupported);
        }

        // + 2 for DST.PORT. The domain form is length-prefixed with NO
        // terminating NUL, so the on-wire size is exactly 1 + len — reading
        // len + 1 here is the off-by-one that walks off the end.
        if (m_buffer.size() < addressAt + addressBytes + 2) {
            return m_phase;
        }

        const std::uint8_t* address = m_buffer.data() + addressAt;
        switch (m_addressType) {
        case kIPv4:
            m_host = formatIPv4(address);
            break;
        case kIPv6:
            m_host = formatIPv6(address);
            break;
        default:
            m_host.assign(reinterpret_cast<const char*>(address), addressBytes);
            break;
        }

        // DST.PORT is two octets "in network octet order".
        const std::size_t portAt = addressAt + addressBytes;
        m_port = (m_buffer[portAt] << 8) | m_buffer[portAt + 1];
        m_buffer.erase(m_buffer.begin(),
                       m_buffer.begin() + static_cast<std::ptrdiff_t>(portAt) + 2);

        // Stops HERE rather than replying: the reply has to say whether the
        // connection succeeded, and nothing has been attempted yet.
        m_phase = Phase::Ready;
        return m_phase;
    }
}

void Handshake::finish(bool ok, std::vector<std::uint8_t>* reply) {
    if (m_phase != Phase::Ready) {
        return;
    }
    appendReply(reply, ok ? kSucceeded : kConnectionRefused);
    m_phase = ok ? Phase::Done : Phase::Failed;
}

}  // namespace krait::net::socks5
