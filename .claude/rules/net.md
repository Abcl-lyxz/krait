---
paths:
  - "src/net/**"
---

# Network & backend rules (src/net/ — security-sensitive)

- **All remote input is hostile.** Length-check before every read; cap all
  remotely-influenced allocations; no format-string with remote data; rate-
  limit terminal answerbacks (DSR/DA floods). New message handling ships with
  fuzz seeds.
- **libssh only** (ADR-0002). libssh2 is banned; `ssh.exe` subprocesses are
  banned. Jump hosts = our own chained `direct-tcpip` channels.
- **Host keys:** never auto-accept. New key → TOFU flow with fingerprint +
  randomart; changed key → blocking per-tab UI showing old vs new with
  plain-language explanation. No "disable checking" setting exists, ever.
- **Secrets:** DPAPI vault only. Never in TOML, never in logs, never in error
  strings, never in memory longer than needed (zero after use). Every log
  call in this directory goes through the redacting logger.
- **Crypto policy:** defaults mirror current OpenSSH defaults; legacy
  algorithms exist only behind per-profile opt-in with a visible warning
  badge. The algorithm table lives in one file with links to its evidence.
- **Clipboard/paste boundary:** bracketed paste always on when the app
  requests it; strip ESC/C0 from pastes; OSC 52 *read* requires explicit
  per-session user permission; cap OSC 52 sizes.
- **Every backend implements `IBackend`** and passes the shared contract
  tests: connect, auth flows, half-close, peer-vanish, flood, reconnect
  policy, and error taxonomy mapping (per-tab banner codes — never dialogs).
- Blocking network calls never run on the UI thread; every network wait has
  a timeout and a cancel path wired to tab close.
