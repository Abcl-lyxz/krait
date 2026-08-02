# Memory index

- [HomeCenter baseline quirks](machine-homecenter-baselines.md) — WARP baselines are vsync-pinned ceilings; machine drifts ~36% between sessions
- [Bench harness invocation](bench-harness-invocation.md) — no bench target; env vars inside krait-app.exe + hidden `[bench]` tag, and the cmd quoting trap
- [Perf audit method](perf-audit-method.md) — release preset only, never touch baselines, attribute via in-session A/B against main
- [Regression history](regression-history.md) — past verdicts and real causes; every one so far was environment, not code
