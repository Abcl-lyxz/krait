---
name: release
description: Cut a Krait release — version bump, changelog, full verification, packaging, tag, GitHub release. Manual-only; never run without the user explicitly asking for a release.
disable-model-invocation: true
allowed-tools: "Read, Grep, Glob, Edit, Write, Bash"
---

Releases are boring on purpose. Refuse to continue past any RED gate; there
is no "release anyway".

Pre-M1 note: until the plan lands packaging infrastructure, steps 5-6 are
design placeholders — execute what exists, report what doesn't yet.

1. **Preconditions.** Clean tree on `main`, `/preflight` GREEN, CI green on
   HEAD. Any BLOCKING finding from a fresh `/review` of unreleased changes
   stops the release.

2. **Security sweep.** Check libssh (and other vcpkg-pinned deps) for
   advisories newer than our pinned versions — `gh` search + vendor pages
   (ADR-0002 obligation). A pending critical CVE in a dep stops the release.

3. **Version.** Bump the single VERSION source (CMake project version —
   the only place a version number lives). Semver: breaking config format =
   major once 1.0 ships; until then minor = milestone, patch = fixes.

4. **Changelog.** Generate CHANGELOG.md section from
   `git log <last-tag>..HEAD --oneline`, grouped: Features / Fixes /
   Performance / Security. Human-readable sentences, not commit subjects.
   User-facing wording — a PuTTY refugee must understand every line.

5. **Build + package.** Release preset (RelWithDebInfo + symbols archived),
   `windeployqt`, LGPL compliance check (libssh + Qt as separate DLLs,
   license texts bundled), installer + portable zip, and the winget manifest
   bump. Sign if the cert is configured; a missing cert is a visible warning
   in the report, not a silent skip.

6. **Smoke test the artifact.** Install the built installer in a temp
   location, run it, open one local shell, one SSH connection to the test
   fixture. The package smoke test is the gate — a green build with a broken
   installer has happened to everyone.

7. **Ship.** `git tag vX.Y.Z` + push (will trigger the ask-gate — that is
   correct), `gh release create` with the changelog section + artifacts.

8. **Close the loop.** Update STATE.md (`/handoff` style) and open the next
   milestone's tracking issue. Report: version, artifacts, checksums, and
   anything skipped with why.
