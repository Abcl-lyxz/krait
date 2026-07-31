#include "telnet_backend.h"

#include <utility>

namespace krait::net {

TelnetBackend::TelnetBackend(TelnetConfig config, QObject* parent)
    : TcpBackend(std::move(config.tcp), parent), m_settings(std::move(config.settings)) {}

void TelnetBackend::resetCodec() {
    // A FRESH negotiator per connection attempt, carrying the grid size the
    // base already knows. Reusing one across a reconnect would start the new
    // session believing options the PREVIOUS server agreed to, which the new
    // one never did — and taking the size from cols()/rows() here is what
    // stops the first NAWS reporting the 80x24 default whatever the window
    // actually is.
    m_settings.cols = cols();
    m_settings.rows = rows();
    m_negotiator = std::make_unique<telnet::Negotiator>(m_settings);
}

void TelnetBackend::handshake(std::vector<std::uint8_t>* reply) {
    m_negotiator->start(reply);
}

void TelnetBackend::decode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                           std::vector<std::uint8_t>* reply) {
    m_negotiator->feed(bytes, data, reply);
}

void TelnetBackend::encode(std::span<const std::uint8_t> bytes,
                           std::vector<std::uint8_t>* out) const {
    m_negotiator->encodeInput(bytes, out);
}

void TelnetBackend::windowChanged(int cols, int rows, std::vector<std::uint8_t>* reply) {
    if (m_negotiator == nullptr) {
        return;  // a resize before the first connection attempt
    }
    m_negotiator->resize(cols, rows, reply);
}

}  // namespace krait::net
