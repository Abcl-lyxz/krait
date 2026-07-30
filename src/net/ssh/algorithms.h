#pragma once

namespace krait::net {

// THE algorithm policy. rules/net.md: "defaults mirror current OpenSSH
// defaults; legacy algorithms exist only behind per-profile opt-in with a
// visible warning badge. The algorithm table lives in one file with links to
// its evidence." This is that file.
//
// Evidence:
//  - OpenSSH defaults: https://man.openbsd.org/ssh_config (KexAlgorithms,
//    Ciphers, MACs, HostKeyAlgorithms)
//  - libssh's own defaults: include/libssh/kex.h in the 0.12 tree
//
// Order matters: each is a preference list, best first, and the server picks
// the first entry it also supports.

// Key exchange.
//
// NOTE the omission. libssh 0.12 advertises `mlkem768x25519-sha256` and
// OpenSSH 10 prefers it — but in this build the client negotiates it and then
// fails with "Failed to construct client init buffer" before sending its KEX
// init. Reproduced against libssh 0.12.0 + OpenSSL 3.6.3 on MSVC, with the
// protocol log captured while writing the T43 contract tests. Advertising an
// algorithm we cannot actually perform means every connection to a modern
// server dies at the handshake, so it is excluded until it is verified working.
//
// This is a POSTPONEMENT, not a preference. Post-quantum key exchange is where
// the whole ecosystem is going, and the moment libssh's PQ path works here it
// goes back at the FRONT of this list. Re-check on the next libssh bump — the
// T43 contract tests are the check.
constexpr const char* kKeyExchange = "curve25519-sha256,"
                                     "curve25519-sha256@libssh.org,"
                                     "ecdh-sha2-nistp521,"
                                     "ecdh-sha2-nistp384,"
                                     "ecdh-sha2-nistp256,"
                                     "diffie-hellman-group-exchange-sha256,"
                                     "diffie-hellman-group16-sha512,"
                                     "diffie-hellman-group18-sha512,"
                                     "diffie-hellman-group14-sha256";

// Ciphers, AEAD first. No CBC and no arcfour: OpenSSH removed them, and a
// terminal session is exactly the traffic the CBC padding-oracle work targeted.
constexpr const char* kCiphers = "chacha20-poly1305@openssh.com,"
                                 "aes256-gcm@openssh.com,"
                                 "aes128-gcm@openssh.com,"
                                 "aes256-ctr,"
                                 "aes192-ctr,"
                                 "aes128-ctr";

// MACs, encrypt-then-MAC only. The plain (MAC-then-encrypt) variants are left
// out for the same reason OpenSSH deprioritises them; the AEAD ciphers above
// carry their own integrity and never consult this list at all.
constexpr const char* kMacs = "hmac-sha2-256-etm@openssh.com,"
                              "hmac-sha2-512-etm@openssh.com";

// Host key types we will accept from a server. ssh-rsa (SHA-1) is absent:
// OpenSSH disabled it by default in 8.8, and it names the SIGNATURE algorithm
// rather than the key, so an RSA host key still works through rsa-sha2-*.
constexpr const char* kHostKeys = "ssh-ed25519,"
                                  "ecdsa-sha2-nistp521,"
                                  "ecdsa-sha2-nistp384,"
                                  "ecdsa-sha2-nistp256,"
                                  "rsa-sha2-512,"
                                  "rsa-sha2-256";

// Our own key types for publickey auth. Same list, same reasoning.
constexpr const char* kPublicKeyTypes = kHostKeys;

}  // namespace krait::net
