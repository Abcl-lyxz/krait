# Krait test strategy

Framework: **Catch2 v3** (ADR-0005), discovered into CTest via
`catch_discover_tests`; `ctest --preset dev` runs everything in the fast tier.

## 1. Tiers

| Tier | Contents | Runs |
|---|---|---|
| unit | pure-function tests next to each module (`tests/unit/`) | per commit |
| corpus | golden-file VT conformance (`tests/corpus/`) | per commit |
| contract | IBackend shared suite per backend (`tests/contract/`) | per commit (conpty), nightly (sshd docker, from M2) |
| golden-image | renderer output hashes on WARP (`tests/render/`) | per commit |
| fuzz | libFuzzer parser target | 60 s smoke per commit, 30 min nightly |
| ASan | full unit+corpus under `/fsanitize=address` | nightly |
| bench | google benchmark vs committed baselines | nightly + on demand |

## 2. Corpus tests (the moat — ADR-0003)

Format (defined in T4, `tests/corpus/**/*.case`):

```
# name: csi_cup_clamps_to_grid
IN  \x1b[999;999H
EXPECT cursor 23 79        # 24x80 default grid, 0-based
```

`IN` = escaped byte string; `EXPECT` = parser-event or grid-state assertions
executed by the harness. Every sequence family ships valid + malformed +
interrupted-mid-sequence variants in the same commit as its implementation
(`rules/vt-core.md`; `/add-escape-sequence` walks it), plus a
`docs/conformance.md` row and a fuzz seed.

**Provenance rules (license-verified):**
- vttest is BSD → deriving cases from its screens/behaviors is fine; scripted
  `tools/vttest-check.cmd` compares implemented-section screens to goldens.
- **esctest is GPL-2.0 → never translate its cases into `tests/`.** It runs
  only as an external harness against a built krait binary (optional nightly
  job, results informational). The boundary is stated in ADR-0005; a violation
  relicenses our corpus.
- xterm ctlseqs + vt100.net are the semantic references for hand-written cases.

## 3. Fuzzing (ADR-0010)

- Target: `tests/fuzz/parser_fuzz.cpp` — bytes → UTF-8 decoder → parser →
  grid, with invariant asserts (cursor in bounds, damage within grid, OSC/DCS
  caps respected, no allocation on hot path via counting allocator hook).
- **Toolchain: clang-cl** (LLVM for Windows) `-fsanitize=fuzzer,address` — the
  reliable path. MSVC `/fsanitize=fuzzer` is officially *experimental* (VS2022
  17.0+; verified learn.microsoft.com): kept as a nightly variant only.
  MSVC ASan (`/fsanitize=address`) is supported and runs the nightly ASan tier;
  incompatible with /RTC and incremental linking — the `asan` preset disables
  both.
- Seeds: every corpus `IN` payload is a seed. Regressions: any crash input is
  committed to `tests/fuzz/regressions/` and replayed per commit forever.
- A parser crash from remote bytes is a **security bug** (rules/vt-core.md):
  fix + regression seed + conformance note in the same commit.

## 4. Golden-image render tests

- Offscreen QRhi with the **D3D11 WARP adapter** (deterministic software
  rasterization; QQuickRhiItem cannot use Qt's software SG backend — verified).
- Readback → hash; goldens per scenario (`ascii_grid`, `sgr_colors`,
  `thai_shaping`, `emoji_vs16`, `cursor_styles`, `underline_styles`).
- Hash mismatch dumps PNG pairs as CI artifacts for eyeballing.
- Real-GPU rendering is covered by bench + manual demo scripts, not goldens
  (driver variance makes exact-hash on hardware flaky by design).

## 5. Benchmarks (`bench/`)

- google benchmark harness; scenarios: parser byte loop (plain text, SGR-heavy,
  scroll flood), grid reflow, damage coalescing, shaper cache hit/miss, full
  flood fps (the M0 spike harness, kept alive as `bench/spike`).
- Baselines committed at `bench/baselines/*.json`:
  `{ "scenario": "parser_plain", "machine": "<ref-id>", "date": "2026-08-01",
  "ns_per_byte": 1.9 }` — one file per machine id; the reference machine is
  recorded in the file, compared only like-for-like.
- `perf-auditor` subagent runs compare; **>5% regression on any baseline fails
  the nightly** and blocks release (rules/render.md budgets are release gates).

## 6. krait-core zero-dependency proof

`tests/core-standalone/` configures `krait-core` alone with
`CMAKE_DISABLE_FIND_PACKAGE_Qt6=ON` and an include-audit script
(`tools/check-core-includes.py`: fails on any `#include <Q...>`, winsock,
windows.h, libssh, hb/ft inside `src/core/`). Runs per commit in CI; breaking
it is breaking the build (rules/vt-core.md).

## 7. CI matrix (ADR-0008)

| Job | Trigger | Runner | Steps |
|---|---|---|---|
| fast-gate | PR + push to main (required) | windows-latest (VS2026) | configure, build, unit+corpus+contract(conpty)+golden-image, fuzz 60 s smoke, clang-format check, clang-tidy on changed files, core-standalone proof |
| nightly-heavy | cron | windows-latest | ASan suite (MSVC), fuzz 30 min (clang-cl), bench vs baselines, full clang-tidy, esctest external run (informational) |
| release | tag | windows-latest | fast-gate + nightly-heavy + windeployqt package + portable zip; later: NSIS + signing (M6) |

Qt 6.11.1 via install-qt-action (`cache: true`); vcpkg binary caching via
GitHub Actions cache. Qt version bumps are deliberate PRs (QRhi has no compat
guarantee — verified), never floating.

## 8. What is NOT tested automatically (and its manual script)

IME composition (scripted manual demo per milestone), real-GPU visual quality
(demo scripts), multi-monitor DPI drag (scripted manual), serial hardware
(com0com covers logic; real adapters manual per release). Each script lives in
`docs/plan/01-milestones.md` acceptance blocks.
