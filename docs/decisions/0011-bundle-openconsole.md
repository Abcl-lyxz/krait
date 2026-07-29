# ADR-0011: Bundle OpenConsole.exe + conpty.dll (pinned, MIT-complied)

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), verification 2026-07-29

## Context

Inbox conhost lags years behind; ConPTY DCS passthrough was only fixed in the
rewritten host around Windows Terminal v1.22 (microsoft/terminal#17313 →
PR #17510) and never backports to inbox. WezTerm's working precedent bundles
exactly `OpenConsole.exe` + `conpty.dll` (assets/windows/conhost).
microsoft/terminal is MIT-licensed — redistribution is permitted with the
copyright notice + license text shipped alongside.

## Decision

- Ship pinned `OpenConsole.exe` + `conpty.dll` in `third_party/openconsole/`
  with Microsoft's LICENSE and a `VERSION.md` recording the exact
  microsoft/terminal release/commit (must be ≥ the v1.22 passthrough fix).
- The ConPTY backend loads our bundled conpty.dll, never the inbox one.
- Updates are deliberate: bump = PR with VERSION.md change + conpty contract
  suite + manual `dir`/vttest smoke.
- Build-vs-download decision (build from source like WezTerm, or repackage a
  release artifact) is made in T15 when we see what current releases contain.

## Alternatives considered

- Inbox ConPTY → stale, DCS holes, mouse-mode gaps; the documented landmine.
- Winpty → obsolete predecessor.

## Consequences

We own tracking upstream OpenConsole releases (release checklist item);
packaging must ship the MS license text (with ADR-0004's third-party
licenses). Local-shell VT capability = whatever our pinned build passes
through — recorded per bump in docs/conformance.md notes. Revisit trigger:
ConPTY passthrough reaching inbox Windows at our floor version.
