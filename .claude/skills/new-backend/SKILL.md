---
name: new-backend
description: Add a new connection backend (protocol) implementing the IBackend contract — lifecycle, flow control, reconnect policy, error taxonomy, contract tests, profile schema, session-manager wiring. Use when implementing or heavily modifying ssh, conpty, telnet, raw, or serial backends in src/net.
allowed-tools: "Read, Grep, Glob, Edit, Write, Bash"
---

Backends are the product's spine and its attack surface. Every backend is
boringly identical from the outside — that is the point. Read
`.claude/rules/net.md` first; it governs everything here.

- [ ] **1. Contract.** Implement `IBackend` exactly: lifecycle
  (resolve → connect → authenticate → stream → half-close → teardown),
  cancellation at every stage (wired to tab close), backpressure/flow
  control, and resize propagation. No extra public surface without an ADR.

- [ ] **2. Error taxonomy.** Map every failure to the shared error codes
  (per-tab banner + reconnect-policy input). A backend inventing its own
  error strings breaks the UI contract. Auth failures vs network failures vs
  protocol violations are distinct codes.

- [ ] **3. Reconnect policy.** Declare what is resumable: serial reconnects
  on replug (VID/PID match), SSH reconnects with backoff (never re-prompting
  saved credentials more than policy allows), raw/telnet per profile flag.
  The policy engine drives it — the backend only reports capability.

- [ ] **4. Threading.** All IO off the UI thread; every wait has a timeout;
  the stream hands chunks to the session's parser thread — never parse
  in-place on the IO thread.

- [ ] **5. Contract tests.** Copy the shared `IBackend` test battery and
  make it pass: connect/auth happy path, peer-vanish mid-stream, flood
  (backpressure holds, UI stays live), half-close semantics, cancel at every
  lifecycle stage, reconnect policy honored. Add protocol-specific fixtures
  (sshd container, telnet negotiation script, loopback COM pair, socat).

- [ ] **6. Profile schema.** Add backend-specific profile fields via
  `/add-setting` (they are settings — registry, EN+TH search, migration).

- [ ] **7. Wiring.** Session manager: protocol picker, quick-connect syntax
  (`krait <proto>://...`), importer mapping if a competitor stores this
  protocol. Docs: one page per backend.

- [ ] **8. Security pass.** Untrusted-input review per rules/net.md, fuzz
  seeds for any parsing (telnet negotiation!), then `/review` (which brings
  in cpp-reviewer) and `/preflight`.
