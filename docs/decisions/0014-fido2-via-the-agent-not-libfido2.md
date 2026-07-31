# ADR-0014: FIDO2 security keys go through the agent, not through libfido2

- **Status:** accepted
- **Date:** 2026-07-31
- **Deciders:** Kla (project owner); spike run against this tree's own libssh
  build and the vcpkg port, 2026-07-31

## Context

M3 lists "FIDO2 keys (libssh 0.12 sk API + libfido2 — prototype task first)".
The prototype ran. What it found:

- **libssh 0.12 genuinely has the sk API.** `SSH_KEYTYPE_SK_ECDSA`,
  `SSH_KEYTYPE_SK_ED25519`, their `_CERT01` variants, `ssh_key_get_sk_flags`,
  `ssh_pki_ctx_set_sk_pin_callback`, `SSH_PKI_OPTION_SK_CALLBACKS` and
  `ssh_sk_resident_keys_load` are all declared in the installed `libssh.h`.
  Reading the header is what makes this look like a solved problem.
- **The implementation is compiled out of the build we link.** libssh gates it
  on `WITH_FIDO2`, which defaults to `OFF`, and vcpkg's port does not turn it
  on: its features are `pcap`, `server`, `zlib`, and its portfile passes no
  `-DWITH_FIDO2`. Verified in this tree rather than from documentation —
  `build/fuzz-msvc/.../blds/libssh/x64-windows-rel/config.h` leaves both
  `WITH_FIDO2` and `HAVE_LIBFIDO2` undefined, and the port's own configure log
  prints `With FIDO2/U2F support: OFF`.
- The declarations are **not** guarded by `WITH_FIDO2`, which is why a security
  key parses and then fails to sign several layers down, with a message about
  nothing the user wrote.

Turning it on means an overlay port for libssh (add a `fido2` feature,
`-DWITH_FIDO2=ON`, depend on vcpkg's `libfido2`), a new transitive dependency
chain (libcbor, and a second OpenSSL edge), and a source build of libssh in
every CI run. It also could not be tested here: no authenticator was available
on this machine, so the whole thing would ship on the strength of it compiling.

## Decision

**Do not add libfido2. Reach security keys through the OpenSSH agent instead.**

The agent already does the FIDO2 work — it holds the key handle, talks to the
authenticator, and produces the signature. A client that speaks the agent
protocol needs libfido2 for none of it; it sends `SSH_AGENTC_SIGN_REQUEST` and
gets a signature back. T60's named-pipe bridge is therefore the FIDO2 story on
Windows, and it costs no new dependency.

Two things follow, and both shipped with this ADR:

1. `SshBackend::tryPublicKey` checks `ssh_key_type()` after importing a named
   key and refuses the sk types with a message naming `ssh-add`, instead of
   letting the user watch a signing failure they cannot act on.
2. `docs/backends.md` says which route works, rather than implying both do.

## Alternatives considered

- **Overlay port with `WITH_FIDO2=ON`.** The literal reading of the milestone
  item. Rejected for now: it buys the direct-from-file path only, which is the
  rarer one on Windows, at the cost of a source build of libssh and a
  dependency we could not exercise.
- **Ship the sk API surface untested and hope.** Rejected. The header compiles
  either way, which is exactly what makes this failure mode expensive.
- **Say nothing and let the error surface.** Rejected: "refused every
  authentication method" for a plugged-in security key is the kind of message
  that produces a bug report and a lost afternoon.

## Consequences

- A security key works when it is loaded into the OpenSSH agent, and Krait says
  so when it is not. **Untested against real hardware** — no authenticator was
  available. What IS tested is that the bridge relays agent traffic byte for
  byte; what is ASSUMED is that an sk signature comes back through it like any
  other.
- `sk-*` keys named directly by a profile are refused, deliberately.
- **Revisit when vcpkg's libssh port gains a FIDO2 feature**, or when a security
  key is available to test with. The refusal in `tryPublicKey` is the thing to
  delete first; it carries this ADR's number so it can be found.
- libssh does **not** verify host certificates: `knownhosts.c` skips
  `@cert-authority` and `@revoked` lines at parse time, and
  `ssh_session_is_known_server` is not CA-aware. A host presenting a CA-signed
  key is treated as an unknown host and goes through the normal TOFU flow. USER
  certificates are unaffected and do work (T61).
