#pragma once

#include "../tcp_backend.h"

#include <cstdint>
#include <span>
#include <vector>

namespace krait::net {

// A TCP socket with NOTHING on top (plan T55).
//
// PuTTY calls this "raw" and it earns its place: half of network debugging is
// wanting to see exactly what a port says, with nothing helpfully rewriting it.
// The moment this file interprets a byte it has stopped being that tool.
//
// So both codec hooks are copies, and the value of this class is the code it
// does NOT contain: no negotiation, no line-ending translation, no escaping.
// Everything real — connect, timeout, retry, the error taxonomy, the write cap
// — is TcpBackend's, shared with telnet.
class RawBackend : public TcpBackend {
    Q_OBJECT

  public:
    explicit RawBackend(TcpConfig config, QObject* parent = nullptr);  // owned by parent

  protected:
    // Straight through. In particular 0xFF is NOT doubled and CR is NOT given
    // a companion NUL: those are telnet's rules, and applying them here would
    // silently corrupt the one protocol whose contract is that nothing is
    // applied.
    void decode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                std::vector<std::uint8_t>* reply) override;
    void encode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* out) const override;
};

}  // namespace krait::net
