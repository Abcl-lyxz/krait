#include "raw_backend.h"

#include <utility>

namespace krait::net {

RawBackend::RawBackend(TcpConfig config, QObject* parent) : TcpBackend(std::move(config), parent) {}

void RawBackend::decode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* data,
                        std::vector<std::uint8_t>* /*reply*/) {
    // Everything is terminal output and nothing is ever answered. A raw socket
    // has no protocol to reply to, and a backend that spoke unprompted would be
    // writing bytes the user never typed into a session they are using to find
    // out what the other end does.
    data->insert(data->end(), bytes.begin(), bytes.end());
}

void RawBackend::encode(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* out) const {
    out->insert(out->end(), bytes.begin(), bytes.end());
}

}  // namespace krait::net
