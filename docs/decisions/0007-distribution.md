# ADR-0007: Distribution = portable ZIP first; NSIS + winget at 1.0; no MSIX

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), plan-session interview + verification

## Context

winget accepts exe/MSI/MSIX installers referenced from a manifest PR at a
stable URL; silent install is mandatory; unsigned exes risk Defender/
SmartScreen rejection (verified learn.microsoft.com + winget-pkgs, 2026-07).
MSIX sandboxing complicates COM-port access, DPAPI paths, and shell
integration — bad fit for a terminal. NSIS 3.12 (2026-04) is maintained;
WiX v6 is stable but heavier to author.

## Decision

- Portable ZIP from the first public M1 build (config-next-to-exe mode).
- NSIS installer at M6/1.0: per-user default, silent-capable, ships LGPL DLLs
  (Qt, libssh) + third-party licenses.
- Code-signing certificate acquired before 1.0; winget manifest PR at 1.0.
- No MSIX for 1.0.

## Alternatives considered

- WiX MSI → enterprise GPO story; add post-1.0 if enterprises ask.
- MSIX → sandbox risk to serial/DPAPI/shell-integration; rejected for 1.0.
- Portable-only → no `winget install krait`; weaker product signal.

## Consequences

M6 gains installer + signing tasks; release checklist gains SmartScreen check
on a clean VM. Revisit trigger: enterprise deployment demand (add MSI) or
MSIX sandbox exemptions maturing.
