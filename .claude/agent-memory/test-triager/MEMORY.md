# Memory index

- [Battery harness](battery-harness.md) — `cmd.exe /c "..."` from Git Bash silently no-ops; use `MSYS_NO_PATHCONV=1 cmd.exe /c '...'` + per-step .cmd scripts
- [Benign signatures](benign-signatures.md) — vswhere/qpa.screen/D9025 noise, Grep's `\`-for-`//` artifact, and the no-op build that fakes "zero warnings"
- [D9025 std override](d9025-std-override.md) — open: 14x c++17→c++23preview warnings in krait-app; /WX does not catch D-series
- [Suite health](suite-health.md) — no flaky tests known; count history (284 @ T52), the scrollback test = ~77% of runtime, bench variance
- [i18n gate false green](i18n-gate-false-green.md) — the [i18n] tests check .ts files against each other only; a missing translation passes
