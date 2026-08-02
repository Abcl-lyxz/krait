---
name: bench-harness-invocation
description: How to actually run Krait's two benchmarks — env-var flood inside krait-app.exe and the hidden shaper [bench] tag — plus the cmd quoting trap
metadata:
  type: reference
---

There is **no separate bench target**. Both benchmarks live inside normal
binaries, driven by environment variables.

**Flood (the render gate).** `bench/baselines/{t25-renderer,m1-wrap,m2-wrap}.json`
all record this one workload: 4K fixed colour buffer, 240x63 grid, every cell
rewritten every frame.

```
KRAIT_TERM_BENCH=600 KRAIT_BENCH_4K=1 KRAIT_BENCH_WARP=1 \
KRAIT_BENCH_OUT=<file.json>  build\release\src\app\krait-app.exe
```

`KRAIT_BENCH_WARP=1` picks the software adapter; `KRAIT_GPU=hardware` picks
D3D11 (which does not work here — see [[machine-homecenter-baselines]]).
Needs `C:\Qt\6.10.3\msvc2022_64\bin` on PATH. Writes JSON only; the Qt app
prints nothing to a piped console, so the JSON file is the whole result.
Exit 2 means the 60 s watchdog fired, i.e. no frames were ever presented.

**Shaper.** Hidden behind Catch2's `[.]` tag, in the core test binary:
`build\release\tests\unit\krait-core-tests.exe [bench]` prints a
`[bench] runs=... cold=... warm=...` line. Its baseline `t23-shaper.json` is a
**Debug** recording (cold 4.352 ms) and the file itself says a release
baseline must replace it before it can gate anything — release cold is ~0.85 ms
warm, so the two are not comparable.

**cmd trap that silently corrupts output paths:** `set VAR=value && prog`
puts the space before `&&` *inside* the value, so `KRAIT_BENCH_OUT` gained a
trailing space and the JSON landed at `foo.json ` (with the space) while every
later read of `foo.json` said "missing". Always use the quoted form:
`set "VAR=value"`. Writing a small `.cmd` wrapper per run is the reliable way.
