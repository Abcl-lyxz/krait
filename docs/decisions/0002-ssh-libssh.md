# ADR-0002: libssh (dynamic, LGPL) for SSH; libssh2 banned; jump hosts DIY

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), research in `docs/research/landscape-2026.md §4`

## Context

An SSH *client* connects to untrusted servers by definition; the SSH library
is the single most security-critical dependency. In June 2026 libssh2
disclosed CVE-2026-55200: a malicious-server RCE, CVSS 9.2, affecting all
versions ≤1.11.1, with a public PoC and no released fix at disclosure — plus
CVE-2026-55199 and CVE-2025-15661 in the same window. libssh2 also lacks
FIDO2/sk keys, OpenSSH certificates, and ssh_config parsing. Feature-wise the
product needs: agent interop, sk-ed25519/sk-ecdsa, certificates, known_hosts
management, keepalives, port forwarding, and jump chains.

## Decision

- **libssh** (LGPL-2.1), **dynamically linked**, via vcpkg.
- FIDO2 keys via `ssh_sk_callbacks` + libfido2. Certificates and ssh_config
  parsing via libssh natively.
- **Jump hosts are implemented in Krait** as chained `direct-tcpip` channels
  (libssh's own ProxyJump support shells out to the OpenSSH binary —
  libssh-mirror issue #178 — which is unacceptable for a GUI client).
  Never shell out to `ssh.exe`.
- Track libssh security advisories in release checklist (`/release` skill);
  pin the version in the vcpkg manifest and bump deliberately.

## Alternatives considered

- libssh2 → disqualified by 2026 CVE track record + missing features.
- OpenSSH subprocess orchestration → no session-level control, terrible
  error UX, Windows OpenSSH lags upstream.
- Rolling our own SSH → absolutely not; crypto protocol code is where
  projects go to die.

## Consequences

- We own the jump-chain engine + its contract tests (sshd test fixtures in
  CI, including multi-hop and failure-mid-chain cases).
- LGPL dynamic linking constrains packaging: ship libssh as a separate DLL,
  keep relink ability; document in `/release`.
- Host-key trust, algorithm policy, and auth flows are our UX code on top of
  libssh primitives — governed by `.claude/rules/net.md`.
- Revisit trigger: a libssh critical CVE pattern emerges, or libssh ships
  native in-process ProxyJump (then delete our chaining code).
