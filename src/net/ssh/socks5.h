#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace krait::net::socks5 {

// The SERVER side of a SOCKS5 handshake, for dynamic (-D) forwards.
//
// No socket in it, for the same reason the telnet negotiator has none: this
// parses bytes from a client we do not control — a browser, curl, or whatever
// else someone points at the port — and that makes it the part a unit test and
// a fuzzer have to be able to reach.
//
// The wire format is RFC 1928; the constants live in the .cpp beside the
// citations rather than here.

enum class Phase : std::uint8_t {
    Greeting,  // reading VER/NMETHODS/METHODS
    Request,   // greeting answered, reading the CONNECT request
    Ready,     // request parsed: host() and port() are set, channel not open yet
    Done,      // final reply sent
    Failed,    // protocol error or an unsupported request; reply already queued
};

class Handshake {
  public:
    // Feeds bytes from the local client, appending any reply to `reply`.
    //
    // Stops at Ready rather than running to completion, because the SOCKS reply
    // has to report whether the connection SUCCEEDED — which is not known until
    // the SSH channel has been attempted. The caller opens the channel and then
    // calls finish().
    Phase feed(std::span<const std::uint8_t> bytes, std::vector<std::uint8_t>* reply);

    // Emits the final reply. `ok` false reports a refused connection, which is
    // what a client needs in order to say "connection refused" rather than
    // hanging.
    void finish(bool ok, std::vector<std::uint8_t>* reply);

    Phase phase() const { return m_phase; }

    // Where the client asked to go. Meaningful from Ready onwards.
    const std::string& host() const { return m_host; }

    int port() const { return m_port; }

  private:
    Phase fail(std::vector<std::uint8_t>* reply, std::uint8_t code);

    Phase m_phase = Phase::Greeting;
    // Bytes seen but not yet consumed. A greeting and a request may arrive in
    // one TCP segment or one byte at a time, and both have to work.
    std::vector<std::uint8_t> m_buffer;
    std::string m_host;
    int m_port = 0;
    // Echoed back in the reply, since the reply's address family follows the
    // request's.
    std::uint8_t m_addressType = 0;
};

}  // namespace krait::net::socks5
