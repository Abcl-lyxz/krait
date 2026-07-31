#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace krait::net {

// Port forwarding specs (plan T59), parsed the way OpenSSH spells them so a
// profile can be copied out of an ssh_config or a wiki page and work.
enum class ForwardKind : std::uint8_t {
    Local,    // -L: listen here, connect from the server
    Remote,   // -R: listen on the server, connect from here
    Dynamic,  // -D: listen here, speak SOCKS, connect from the server
};

struct Forward {
    ForwardKind kind = ForwardKind::Local;
    // Where the listener sits. Empty means loopback — see parseForward for why
    // that is the default rather than "all interfaces".
    std::string bindAddress;
    int bindPort = 0;
    // Where traffic goes. Unused for Dynamic, which learns it per connection
    // from the SOCKS request.
    std::string destHost;
    int destPort = 0;

    // "L 8080 -> internal:80", for the tunnel pane.
    std::string describe() const;
};

// Parses one spec. OpenSSH's forms, all supported:
//
//   -L [bind:]port:host:hostport
//   -R [bind:]port:host:hostport
//   -D [bind:]port
//
// Returns false on anything it cannot read, rather than guessing: a forward
// that silently binds somewhere other than intended is a hole, and the failure
// mode of guessing is a listener on a public interface.
//
// IPv6 literals are bracketed, as in ssh_config: "[::1]:8080:[::1]:80".
bool parseForward(ForwardKind kind, std::string_view spec, Forward* out);

// Parses the `forwards` profile key: one spec per entry, each prefixed by its
// letter — "L 8080:internal:80", "D 1080", "R 9000:localhost:22". Bad entries
// are SKIPPED and named in `rejected` rather than dropping the whole list,
// because one typo should not silently disable the other four tunnels.
std::vector<Forward> parseForwards(std::string_view text, std::vector<std::string>* rejected);

}  // namespace krait::net
