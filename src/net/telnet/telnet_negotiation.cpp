#include "telnet_negotiation.h"

#include <algorithm>

namespace krait::net::telnet {
namespace {

void sendCommand(std::vector<std::uint8_t>* reply, std::uint8_t command, std::uint8_t option) {
    reply->push_back(kIac);
    reply->push_back(command);
    reply->push_back(option);
}

// Appends a payload byte, doubling IAC. RFC 855 requires this INSIDE
// subnegotiations too, which RFC 1073 spells out for a width or height of 255 —
// the case that gets missed because it needs an unusually wide terminal to
// notice.
void pushEscaped(std::vector<std::uint8_t>* out, std::uint8_t byte) {
    out->push_back(byte);
    if (byte == kIac) {
        out->push_back(kIac);
    }
}

}  // namespace

bool Negotiator::allowHim(std::uint8_t option) {
    switch (option) {
    case kEcho:
    case kSuppressGoAhead:
    case kBinary:
        return true;
    default:
        // RFC 1123: "A host MUST refuse ... an unsupported option". Refusing by
        // default rather than listing what to refuse means an option added to
        // the registry tomorrow is already handled.
        return false;
    }
}

bool Negotiator::allowUs(std::uint8_t option) {
    switch (option) {
    case kTerminalType:
    case kNaws:
    case kSuppressGoAhead:
    case kBinary:
        return true;
    // The next case is identical to `default` by arithmetic and entirely
    // different by intent. Folding it in would delete the reason echo is
    // refused, and the next person adding an option would have nothing telling
    // them not to enable it.
    // NOLINTNEXTLINE(bugprone-branch-clone)
    case kEcho:
        // NEVER. RFC 857 warns that both ends echoing "loops back and forth
        // indefinitely", and a client that echoes is also a client that echoes
        // the password the server just asked it not to.
        return false;
    default:
        // kNewEnviron and kAuthentication land here, which is the point:
        // NEW-ENVIRON hands the environment to the far end, and AUTHENTICATION
        // negotiates its own strength in the clear (RFC 2941).
        return false;
    }
}

void Negotiator::start(std::vector<std::uint8_t>* reply) {
    // What we offer, and nothing else. Each of these moves us to WANTYES, so
    // the reply is already accounted for by the Q Method when it arrives.
    for (const std::uint8_t option : {std::uint8_t{kTerminalType}, std::uint8_t{kNaws}}) {
        m_us[option] = OptionState::WantYesEmpty;
        sendCommand(reply, kWill, option);
    }
    for (const std::uint8_t option : {std::uint8_t{kSuppressGoAhead}, std::uint8_t{kEcho}}) {
        m_him[option] = OptionState::WantYesEmpty;
        sendCommand(reply, kDo, option);
    }
}

void Negotiator::feed(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                      std::vector<std::uint8_t>* reply) {
    for (const std::uint8_t byte : bytes) {
        switch (m_phase) {
        case Phase::Data:
            if (byte == kIac) {
                m_phase = Phase::Iac;
            } else {
                data->push_back(byte);
            }
            break;

        case Phase::Iac:
            switch (byte) {
            case kIac:
                // Doubled: one literal 0xFF of application data.
                data->push_back(kIac);
                m_phase = Phase::Data;
                break;
            case kWill:
            case kWont:
            case kDo:
            case kDont:
                m_command = byte;
                m_phase = Phase::Negotiate;
                break;
            case kSb:
                m_phase = Phase::SubOption;
                break;
            default:
                // RFC 1123: "A host MUST be able to receive and ignore any
                // Telnet control functions that it does not support." Both
                // bytes are consumed; nothing reaches the terminal. NOP, GA and
                // Data Mark all land here, which is correct — none of them mean
                // anything to a client that never enters urgent mode.
                m_phase = Phase::Data;
                break;
            }
            break;

        case Phase::Negotiate:
            switch (m_command) {
            case kWill:
                receiveWill(byte, reply);
                break;
            case kWont:
                receiveWont(byte, reply);
                break;
            case kDo:
                receiveDo(byte, reply);
                break;
            default:
                receiveDont(byte, reply);
                break;
            }
            m_phase = Phase::Data;
            break;

        case Phase::SubOption:
            m_subOption = byte;
            m_subData.clear();
            m_subOverflow = false;
            m_phase = Phase::SubData;
            break;

        case Phase::SubData:
            if (byte == kIac) {
                m_phase = Phase::SubIac;
            } else if (m_subData.size() < kMaxSubnegotiation) {
                m_subData.push_back(byte);
            } else {
                // Over the cap. The REST is consumed and thrown away rather
                // than the parser bailing to Data: bailing would treat the
                // remaining parameters — attacker-chosen bytes — as terminal
                // output. RFC 855 sets no limit, so this one is ours.
                m_subOverflow = true;
            }
            break;

        case Phase::SubIac:
            if (byte == kSe) {
                // The ONLY terminator. A bare SE inside parameters is data
                // (RFC 855), so a server cannot cut its own subnegotiation
                // short and have the rest land in the terminal.
                if (m_subOverflow) {
                    ++m_dropped;
                } else {
                    handleSubnegotiation(reply);
                }
                m_subData.clear();
                m_subOverflow = false;
                m_phase = Phase::Data;
            } else if (byte == kIac) {
                // Doubled inside parameters.
                if (m_subData.size() < kMaxSubnegotiation) {
                    m_subData.push_back(kIac);
                } else {
                    m_subOverflow = true;
                }
                m_phase = Phase::SubData;
            } else {
                // IAC followed by something else mid-subnegotiation is
                // undefined by RFC 855. Treated as the subnegotiation being
                // abandoned, because the alternative — resuming as if the IAC
                // were data — leaves the parser trusting a stream that has
                // already contradicted itself.
                ++m_dropped;
                m_subData.clear();
                m_subOverflow = false;
                m_phase = Phase::Data;
            }
            break;
        }
    }
}

// The RFC 1143 tables. `him` on WILL/WONT below; `us` on DO/DONT is the same
// procedure "with DO-WILL, DONT-WONT, him-us, himq-usq swapped", which is why
// the four functions look like two.

void Negotiator::receiveWill(std::uint8_t option, std::vector<std::uint8_t>* reply) {
    switch (m_him[option]) {
    case OptionState::No:
        if (allowHim(option)) {
            m_him[option] = OptionState::Yes;
            sendCommand(reply, kDo, option);
        } else {
            sendCommand(reply, kDont, option);
        }
        break;
    case OptionState::Yes:
        // Already on. RFC 854: not acknowledging is "essential to prevent
        // endless loops in the negotiation" — the silence IS the protocol.
        break;
    case OptionState::WantNoEmpty:
        m_him[option] = OptionState::No;  // protocol error: DONT answered by WILL
        break;
    case OptionState::WantNoOpposite:
    case OptionState::WantYesEmpty:
        // Two DIFFERENT rows of RFC 1143's table that happen to land on the
        // same answer — the first is a queued enable arriving early, the
        // second is our own DO being agreed to. Both states stay named; only
        // the body is shared.
        m_him[option] = OptionState::Yes;
        break;
    case OptionState::WantYesOpposite:
        m_him[option] = OptionState::WantNoEmpty;
        sendCommand(reply, kDont, option);
        break;
    }
}

void Negotiator::receiveWont(std::uint8_t option, std::vector<std::uint8_t>* reply) {
    switch (m_him[option]) {
    case OptionState::No:
        break;  // already off; silence
    case OptionState::Yes:
        m_him[option] = OptionState::No;
        sendCommand(reply, kDont, option);
        break;
    case OptionState::WantNoEmpty:
    case OptionState::WantYesEmpty:
    case OptionState::WantYesOpposite:
        m_him[option] = OptionState::No;
        break;
    case OptionState::WantNoOpposite:
        m_him[option] = OptionState::WantYesEmpty;
        sendCommand(reply, kDo, option);
        break;
    }
}

void Negotiator::receiveDo(std::uint8_t option, std::vector<std::uint8_t>* reply) {
    switch (m_us[option]) {
    case OptionState::No:
        if (allowUs(option)) {
            m_us[option] = OptionState::Yes;
            sendCommand(reply, kWill, option);
            // NAWS is the one option whose whole point is the value that
            // follows it, and RFC 1143 forbids acting on an option before it is
            // enabled — so the size goes out here, immediately after the WILL,
            // and not a moment earlier.
            if (option == kNaws) {
                sendNaws(reply);
            }
        } else {
            sendCommand(reply, kWont, option);
        }
        break;
    case OptionState::Yes:
        break;
    case OptionState::WantNoEmpty:
        m_us[option] = OptionState::No;
        break;
    case OptionState::WantNoOpposite:
        m_us[option] = OptionState::Yes;
        break;
    case OptionState::WantYesEmpty:
        m_us[option] = OptionState::Yes;
        if (option == kNaws) {
            sendNaws(reply);
        }
        break;
    case OptionState::WantYesOpposite:
        m_us[option] = OptionState::WantNoEmpty;
        sendCommand(reply, kWont, option);
        break;
    }
}

void Negotiator::receiveDont(std::uint8_t option, std::vector<std::uint8_t>* reply) {
    switch (m_us[option]) {
    case OptionState::No:
        break;
    case OptionState::Yes:
        m_us[option] = OptionState::No;
        sendCommand(reply, kWont, option);
        break;
    case OptionState::WantNoEmpty:
    case OptionState::WantYesEmpty:
    case OptionState::WantYesOpposite:
        m_us[option] = OptionState::No;
        break;
    case OptionState::WantNoOpposite:
        m_us[option] = OptionState::WantYesEmpty;
        sendCommand(reply, kWill, option);
        break;
    }
}

void Negotiator::handleSubnegotiation(std::vector<std::uint8_t>* reply) {
    // RFC 1143: "During the negotiation state, any effects of having the option
    // enabled MUST NOT be used." A server that sends SB for something we never
    // agreed to gets nothing back — answering would let it drive us with
    // options it only claimed.
    if (m_subOption == kTerminalType && m_us[kTerminalType] == OptionState::Yes) {
        if (m_subData.empty() || m_subData[0] != kTerminalTypeSend) {
            return;
        }
        reply->push_back(kIac);
        reply->push_back(kSb);
        reply->push_back(kTerminalType);
        reply->push_back(kTerminalTypeIs);
        // RFC 1091 caps a terminal type name at 40 characters. Ours is a
        // constant, but it arrives through settings, so it is truncated here
        // rather than trusted.
        const std::size_t limit = std::min<std::size_t>(m_settings.terminalType.size(), 40);
        for (std::size_t i = 0; i < limit; ++i) {
            pushEscaped(reply, static_cast<std::uint8_t>(m_settings.terminalType[i]));
        }
        reply->push_back(kIac);
        reply->push_back(kSe);
    }
    // No other subnegotiation needs an answer: NAWS is client-to-server only,
    // and every option we refuse never reaches Yes.
}

void Negotiator::sendNaws(std::vector<std::uint8_t>* reply) const {
    // RFC 1073: 16-bit big-endian width then height, each byte doubled if it is
    // 255. Clamped because the fields are 16-bit and a grid is not.
    const auto cols = static_cast<std::uint16_t>(std::clamp(m_settings.cols, 0, 0xFFFF));
    const auto rows = static_cast<std::uint16_t>(std::clamp(m_settings.rows, 0, 0xFFFF));
    reply->push_back(kIac);
    reply->push_back(kSb);
    reply->push_back(kNaws);
    pushEscaped(reply, static_cast<std::uint8_t>(cols >> 8));
    pushEscaped(reply, static_cast<std::uint8_t>(cols & 0xFF));
    pushEscaped(reply, static_cast<std::uint8_t>(rows >> 8));
    pushEscaped(reply, static_cast<std::uint8_t>(rows & 0xFF));
    reply->push_back(kIac);
    reply->push_back(kSe);
}

void Negotiator::resize(int cols, int rows, std::vector<std::uint8_t>* reply) {
    m_settings.cols = cols;
    m_settings.rows = rows;
    if (m_us[kNaws] == OptionState::Yes) {
        sendNaws(reply);
    }
}

void Negotiator::encodeInput(std::span<const std::uint8_t> bytes,
                             std::vector<std::uint8_t>* out) const {
    const bool binary = m_us[kBinary] == OptionState::Yes;
    for (const std::uint8_t byte : bytes) {
        if (byte == kIac) {
            // Doubled in both modes: RFC 856 keeps the rule under BINARY, which
            // is the half people drop.
            out->push_back(kIac);
            out->push_back(kIac);
            continue;
        }
        if (!binary && byte == '\r') {
            // NVT: "the sequence CR NUL must be used where a carriage return
            // alone is actually desired" (RFC 854). A bare CR is the one thing
            // the spec says not to send, and a terminal sends CR on every Enter.
            out->push_back('\r');
            out->push_back('\0');
            continue;
        }
        out->push_back(byte);
    }
}

}  // namespace krait::net::telnet
