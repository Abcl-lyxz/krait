#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace krait::net::telnet {

// Telnet option negotiation, with no socket anywhere in it (plan T54).
//
// Separated from the backend for the same reason src/core has no Qt: this is
// the part that parses HOSTILE bytes, so it has to be reachable by a unit test
// and a fuzzer without opening a connection. The backend above it does nothing
// but move bytes between a QTcpSocket and this class.
//
// Every constant and rule here is cited in docs/research/t54-telnet-findings.md,
// which records what the RFCs actually say — including the three things they do
// NOT say, which are the ones that matter for hostile input.

// RFC 854. SB is 250 and SE is 240: they do not bracket each other numerically,
// which is the off-by-one waiting to be written.
enum Command : std::uint8_t {
    kSe = 240,
    kNop = 241,
    kDataMark = 242,
    kBreak = 243,
    kInterrupt = 244,
    kAbortOutput = 245,
    kAreYouThere = 246,
    kEraseChar = 247,
    kEraseLine = 248,
    kGoAhead = 249,
    kSb = 250,
    kWill = 251,
    kWont = 252,
    kDo = 253,
    kDont = 254,
    kIac = 255,
};

enum Option : std::uint8_t {
    kBinary = 0,           // RFC 856, negotiated per direction
    kEcho = 1,             // RFC 857
    kSuppressGoAhead = 3,  // RFC 858
    kAuthentication = 37,  // RFC 2941 — refused, see below
    kNewEnviron = 39,      // RFC 1572 — refused, see below
    kTerminalType = 24,    // RFC 1091
    kNaws = 31,            // RFC 1073
};

// RFC 1091: IS and SEND inside a TERMINAL-TYPE subnegotiation.
inline constexpr std::uint8_t kTerminalTypeIs = 0;
inline constexpr std::uint8_t kTerminalTypeSend = 1;

// RFC 1143's Q Method. The queue bit is folded into the state rather than kept
// beside it, because the two are only ever read together and a separate bool is
// a second thing to keep in step.
enum class OptionState : std::uint8_t {
    No,
    Yes,
    WantNoEmpty,
    WantNoOpposite,
    WantYesEmpty,
    WantYesOpposite,
};

// What the terminal tells the far end about itself.
struct TelnetSettings {
    // RFC 1091: <= 40 characters, case-insensitive. Sent in a TERMINAL-TYPE
    // subnegotiation when the server asks.
    std::string terminalType = "xterm-256color";
    int cols = 80;
    int rows = 24;
};

// Ours, not the RFCs' — RFC 855 imposes no bound on a subnegotiation at all,
// which makes an unbounded buffer here a remote allocation primitive. Big
// enough for any option we answer (NAWS is 4 bytes, a terminal type is 40) with
// room for a server that pads.
inline constexpr std::size_t kMaxSubnegotiation = 1024;

// Feeds the byte stream in and gets application data plus replies out.
//
// Holds no buffer of application data: feed() appends to the caller's, so a
// flood costs one copy rather than growing something in here that nothing
// drains.
class Negotiator {
  public:
    explicit Negotiator(TelnetSettings settings) : m_settings(std::move(settings)) {}

    // Parses `bytes`. Application data is appended to `data`; anything that
    // must go back to the server is appended to `reply`. Never throws, never
    // allocates unboundedly, and never blocks.
    void feed(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
              std::vector<std::uint8_t>* reply);

    // The opening offer: WILL TERMINAL-TYPE, WILL NAWS, DO SGA, DO ECHO.
    // Separate from the constructor so the caller decides when the socket is
    // ready to carry it.
    void start(std::vector<std::uint8_t>* reply);

    // Window size changed. Emits a NAWS subnegotiation if the option is on;
    // silent otherwise, since RFC 1143 forbids using an option's effects before
    // it is enabled.
    void resize(int cols, int rows, std::vector<std::uint8_t>* reply);

    // Encodes application input for the wire: doubles 0xFF (RFC 854), and
    // applies the NVT line-ending discipline unless BINARY is on in our
    // direction.
    void encodeInput(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* out) const;

    // For tests and the status line. `him` is the server's side of the option.
    OptionState him(std::uint8_t option) const { return m_him[option]; }

    OptionState us(std::uint8_t option) const { return m_us[option]; }

    // True once the server has taken over echoing — the signal that a password
    // prompt is on screen, and the reason ECHO is negotiated at all.
    bool serverEchoes() const { return him(kEcho) == OptionState::Yes; }

    // Subnegotiations dropped for exceeding kMaxSubnegotiation. Surfaced rather
    // than swallowed: a server whose options never take effect should be
    // explainable without a packet capture.
    int droppedSubnegotiations() const { return m_dropped; }

  private:
    // Where the byte stream currently is. Named for what has been SEEN, not for
    // what is expected next, so a trace reads in the same direction as the data.
    enum class Phase : std::uint8_t {
        Data,
        Iac,        // saw IAC
        Negotiate,  // saw IAC WILL/WONT/DO/DONT, awaiting the option
        SubOption,  // saw IAC SB, awaiting the option
        SubData,    // inside subnegotiation parameters
        SubIac,     // saw IAC inside parameters
    };

    // Whether we are willing to let the server enable `option`, and whether we
    // want to enable it ourselves. One place each, so a refusal cannot be
    // forgotten in one direction and honoured in the other.
    static bool allowHim(std::uint8_t option);
    static bool allowUs(std::uint8_t option);

    void receiveWill(std::uint8_t option, std::vector<std::uint8_t>* reply);
    void receiveWont(std::uint8_t option, std::vector<std::uint8_t>* reply);
    void receiveDo(std::uint8_t option, std::vector<std::uint8_t>* reply);
    void receiveDont(std::uint8_t option, std::vector<std::uint8_t>* reply);
    void handleSubnegotiation(std::vector<std::uint8_t>* reply);
    void sendNaws(std::vector<std::uint8_t>* reply) const;

    TelnetSettings m_settings;
    Phase m_phase = Phase::Data;
    std::uint8_t m_command = 0;  // the WILL/WONT/DO/DONT being completed
    std::uint8_t m_subOption = 0;
    std::vector<std::uint8_t> m_subData;
    // Set when a subnegotiation overflowed: the rest of it is consumed and
    // discarded rather than treated as data, because the parameters are not
    // ours to interpret halfway through.
    bool m_subOverflow = false;
    int m_dropped = 0;

    // 256 slots rather than a map: the option space IS a byte, so this is two
    // fixed arrays instead of a container with an allocator, and a lookup on
    // the byte path costs an index.
    std::array<OptionState, 256> m_him{};
    std::array<OptionState, 256> m_us{};
};

}  // namespace krait::net::telnet
