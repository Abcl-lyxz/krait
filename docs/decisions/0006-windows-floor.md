# ADR-0006: Minimum Windows = Windows 10 22H2 (19045) and Windows 11

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), plan-session interview

## Context

ConPTY exists since Win10 1809, but we bundle our own OpenConsole (ADR-0011),
which reduces dependence on inbox console quality. Microsoft supports only
Win10 22H2 among Win10 releases today; older builds are unpatched and
untestable for us in practice.

## Decision

Support Windows 10 22H2 (build 19045) and Windows 11. No guards or fallbacks
for older builds; the installer and README state the floor. D3D11 (feature
level 11.0) + WARP assumed present.

## Alternatives considered

- 1903+/1809+ → larger nominal reach, untestable matrix, unpatched systems.
- Win11-only → excludes the Win10 enterprise base that is PuTTY's home turf.

## Consequences

Modern ConPTY, DPAPI, and D3D11.1 APIs assumed unconditionally; CI runs on
Server 2025 which exceeds the floor — a periodic manual smoke on a Win10 22H2
VM is part of the release checklist. Revisit trigger: Win10 22H2 EOL
(Oct 2025 consumer / ESU horizons) — raise the floor when its share among
target users drops.
