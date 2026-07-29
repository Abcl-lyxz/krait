# ADR-0012: Jump hosts via libssh native ProxyJump (partial supersede of ADR-0002)

- **Status:** accepted — supersedes the jump-host portion of ADR-0002 only
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), context7/libssh verification 2026-07-29

## Context

ADR-0002 decided to implement jump chains ourselves via chained direct-tcpip,
citing libssh's ProxyJump shelling out to the OpenSSH binary (issue #178).
That fact is stale: **libssh 0.11.0 (2024-08) added native in-process
ProxyJump** — `SSH_OPTIONS_PROXYJUMP` (comma-separated hops) and
`SSH_OPTIONS_PROXYJUMP_CB_LIST_APPEND` with per-hop
`ssh_jump_callbacks_struct`, letting the client run host-key and auth UX for
every intermediate hop. The ssh.exe path is now only an env-var opt-out
(`OPENSSH_PROXYJUMP=1`). 0.12.0 notes cite ProxyJump stability fixes.
ADR-0002's own revisit trigger ("libssh ships native in-process ProxyJump →
delete our chaining code") has fired before we wrote any chaining code.

## Decision

- Jump chains use libssh native ProxyJump with per-hop callbacks; Krait's
  host-key TOFU/changed-key UX and auth prompts run for every hop
  (rules/net.md applies per hop).
- `ssh_channel_open_forward` (direct-tcpip) remains the mechanism for port
  forwarding and the visual tunnel manager — that part of ADR-0002 stands.
- Everything else in ADR-0002 (libssh over libssh2, dynamic LGPL linking,
  never shell out to ssh.exe) stands unchanged.
- Before M3 hop-UX work: pull the `ssh_jump_callbacks_struct` header to
  confirm field layout (not verified in this pass).

## Alternatives considered

- Keep DIY chaining → duplicates what upstream now maintains; more of our
  security-critical code for no capability gain.

## Consequences

M2/M3 jump-host effort shrinks to UX + contract tests (multi-hop, failure
mid-chain — tests stay as ADR-0002 required). We depend on libssh's chaining
correctness; contract tests against multi-hop sshd fixtures gate it. Revisit
trigger: native ProxyJump proves buggy for our flows (then the direct-tcpip
chaining plan revives from ADR-0002's design).
