#pragma once

#include "../tcp_backend.h"
#include "telnet_negotiation.h"

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace krait::net {

struct TelnetConfig {
    TcpConfig tcp;
    telnet::TelnetSettings settings;
};

// Telnet (plan T54). Everything that is a socket lives in TcpBackend and
// everything that is the protocol lives in telnet::Negotiator, which has no
// socket in it — so this class is only the join between them, and there is
// very little in it to get wrong.
class TelnetBackend : public TcpBackend {
    Q_OBJECT

  public:
    explicit TelnetBackend(TelnetConfig config,
                           QObject* parent = nullptr);  // owned by parent

  protected:
    void decode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                std::vector<std::uint8_t>* reply) override;
    void encode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* out) const override;
    void handshake(std::vector<std::uint8_t>* reply) override;
    void windowChanged(int cols, int rows, std::vector<std::uint8_t>* reply) override;
    void resetCodec() override;

  private:
    telnet::TelnetSettings m_settings;
    std::unique_ptr<telnet::Negotiator> m_negotiator;
};

}  // namespace krait::net
