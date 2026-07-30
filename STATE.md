# STATE

Phase: **M1 in progress** — T17 merged, T18 and T19 built and awaiting merge

## Now

Two stacked branches are ahead of `main`, both fully built and tested locally:

| Branch | Contents | PR | State |
|---|---|---|---|
| `t18-scroll` | T18 complete: DECSTBM, IL/DL/SU/SD, DECOM, alt screen 1049 | **#15** | CI was red twice, both causes fixed, latest run pending at handoff |
| `t19-width` | T19 complete: grapheme/width engine | none yet | branched off `t18-scroll`, **not pushed** |

**First actions next session, in order:**

1. `gh pr checks 15`. If green, `gh pr merge 15 --merge` (the stack has used
   merge commits so far).
2. `git push -u origin t19-width` and open its PR. **Retarget it to `main` with
   `gh pr edit <N> --base main` before merging** — GitHub only auto-retargets a
   stacked PR when the base branch is deleted, which bit this project during M0
   closeout.
3. Then T20 (reflow). See "T20 starts here" below — T19 deliberately left the
   grid untouched, and T20 is where wide characters actually start rendering.

## What T18 and T19 landed

T18 — three commits. `handleScroll` (DECSTBM/IL/DL/SU/SD) plus a new
`handleMode` seam for the `?`-private DECSET/DECRST forms carrying DECOM and
1049. Full behavior notes are in `docs/conformance.md`; the short version is
that xterm's source was treated as the authority over DEC prose wherever they
disagreed, and every such divergence is written down in the ledger rather than
left implicit in the code.

T19 — `src/core/unicode/width.h`, now the ONLY sanctioned way to ask how many
cells something occupies. Nothing consumes it yet, by design.

### Three bugs T18 fixed in already-merged code

1. **CUU/CUD were not margin-aware.** xterm's `CursorUp`/`CursorDown` read the
   scrolling margins and never the flags, so they clamp to the region
   regardless of DECOM. We clamped to the page.
2. **DSR 6 CPR ignored origin mode**, breaking the CPR round trip every
   full-screen app performs. Its subtraction is now clamped at 0 — a 1049
   restore can leave the cursor above the top margin, and `ESC [ -18 ; 1 R`
   contains a byte that is not a CSI parameter byte (ECMA-48 §5.4), so the
   application drops the whole reply and anything blocking on CPR hangs.
3. **The fuzz target routed neither `handleScroll` nor `handleMode`.** So
   `scroll.seed` AND `mode.seed` were inert — T18's fuzz-seed requirement was
   satisfied only on paper. Both are wired now, plus margin and row-width
   invariant asserts.

## Evidence, not assertion

Re-run on the final `t19-width` tree:

| Gate | Result |
|---|---|
| `cmake --build --preset dev` | pass, no warnings beyond the known Qt D9025 noise |
| `ctest --preset dev` | **47/47** |
| `tests\fuzz\run-smoke.cmd` | pass — 174,174 execs, **0 crashes**, exit 0 |
| standalone zero-dep proof, Qt blanked | pass, including utf8proc |
| clang-tidy on every changed TU | clean (see the local-repro recipe below) |
| clang-format | clean |

Corpus grew by ~48 assertions: `csi/origin.case` (31 cases),
`sgr/mode.case` (17), and three DECOM cases added to `reports/basic.case`,
which had **zero** DECOM coverage before. `[width]` adds 11 unit tests.

## Run clang-tidy LOCALLY — it is installed, and I wasted two CI cycles not doing it

`clang-tidy` and `clang-format` are on PATH (LLVM 22.1.0, via the Python
scripts dir). Two red CI runs on PR #15 were both lint-only and both
reproducible locally in ~90 s instead of 5 minutes.

**It must run inside a VS x64 dev shell.** Without MSVC's `INCLUDE`, clang-tidy
cannot find `<cstdint>`, every TU becomes parse garbage, and it reports findings
that do NOT reproduce in CI (`bugprone-branch-clone` false hits, in my case).
The CI file already carries this warning; heed it.

```
# in a VS x64 dev shell, from the repo root
clang-tidy -p build/dev --extra-arg=-Wno-unknown-argument \
  --extra-arg=-Wno-unused-command-line-argument <changed .cpp files>
```

Gating is `.clang-tidy`'s `WarningsAsErrors` (`bugprone-*,concurrency-*`).
Everything else is advisory in the fast gate. Exclude `tests/fuzz/*.cpp`: it is
built only by the fuzz presets, so it has no `compile_commands.json` entry and
falls back to C++17, where every `std::span` becomes a fatal
`clang-diagnostic-error` about nothing. The CI step now filters those out and
logs what it skipped.

## T20 starts here

T20 is reflow, and it is the task T8's grid was built to enable — three
`[!mayfail]` cases in `tests/unit/grid_test.cpp` are already in the tree waiting
to be flipped green.

**T20 also owns the wide-cell work T19 deliberately did not do.** The grid still
stores one codepoint per cell and reserves no second cell for a wide cluster, so
wide characters and multi-codepoint clusters do not render correctly today. That
needs a `Cell` storage decision first — a cluster is base + marks, and `Cell`
currently holds a single `char32_t`. Options are an inline small array or an
interned cluster table; nothing has been decided, and no code assumes either.
`docs/conformance.md`'s "Width & clustering" row states this gap plainly.

There is also a deferred `ponytail:` note in `src/core/grid/cell.h` from T17:
packing `Color` into a uint32 would make `Cell` 20 bytes and save ~19 MB at full
scrollback. T21 (scrollback) is the natural place for it, and it interacts with
whatever cluster storage T20 picks — decide them together.

## Watchouts, all of them earned this session

- **Branch protection is on for `main`.** Commit on a `tN-*` branch and open a
  PR; direct pushes are rejected.
- **Stacked PRs do not auto-retarget.** `gh pr edit N --base main` before merge.
- **ctest TEST_CASE names must stay ASCII.** `catch_discover_tests` round-trips
  the name through a ctest filter; an em-dash gets mangled by the console
  codepage until the test cannot be matched by its own name, and it reports as a
  failure that has nothing to do with the assertions. Cost one debug cycle.
- **`if errorlevel 1` in cmd is a SIGNED comparison** and misses -1
  (`[code=4294967295]`). Use `if %ERRORLEVEL% neq 0`.
- **pwsh does not throw on a native non-zero exit mid-script**, so a failing
  `git` silently turns a lint gate into a green no-op. Check `$LASTEXITCODE`.
- **`bugprone-misplaced-widening-cast` gates the build.** Widen each operand
  before arithmetic (`static_cast<size_t>(r) + 1`), never the sum.
- The corpus harness only understands `\xNN` escapes — **not** `\r\n`.
- **`run-smoke.cmd` now needs `VCPKG_ROOT`** (the fuzz presets gained the vcpkg
  toolchain in T19 so they can resolve utf8proc). It fails with a clear message
  if unset. A `toolchainFile` change does NOT apply to an already-configured
  build dir — delete `build\fuzz-msvc` if the utf8proc error persists.
- **Re-run every gate AFTER the change that could invalidate it.** T19's first
  CI run died in the fuzz step because I had measured the fuzz smoke before
  linking utf8proc and carried the stale number forward into the PR body.

## Known-not-fixed

- `bench/baselines/m0-spike.json` is ~1.9x stale in the FAVOURABLE direction
  (WARP measured 152.4 fps vs 80.7 recorded), so M1's ">5% regression" gate is
  miscalibrated and will not catch a real regression. Re-baseline before T25.
- ADR-0010's clang-cl `fuzz` preset cannot link (`lld-link` ignores
  `-fsanitize=fuzzer,address`); needs explicit `clang_rt` import libs. Default
  is `fuzz-msvc`, which works.
- Qt is pinned to 6.10.3, not the plan's 6.11.1 — aqtinstall 3.3.0 cannot
  resolve 6.11's per-arch repo layout.
- 14x D9025 `/std` override warnings from Qt's `cxx_std_17` INTERFACE compile
  feature. Cosmetic.
- vttest is still not wired up (`tools/vttest-check.cmd` is T35). Do not claim
  vttest conformance.
- 1049's clear uses default cells where xterm fills with the current pen.
  Deferred deliberately so BCE stays ONE decision across ED/EL/IL/DL/SU/SD/1049.
- `?7` DECAWM and `?25` DECTCEM are accepted but not honored — declared in the
  ledger as modes that make output wrong rather than merely absent.
- T19 wrote no `tools/gen-width-tables.py`. Deliberate plan deviation: utf8proc
  2.11.3 ships UCD 17.0.0 already. Reasoning and the measured probe output are
  in `docs/research/t19-width-findings.md`.

## Done so far

T1-T16 = M0, complete and merged (see git history; ADR-0013 records the GO
verdict). M1: T17 merged (SGR extended). T18 and T19 as described above.

Task numbering: **M0 = T1-T16, M1 = T17-T35**, both in
`docs/plan/02-m0-tasks.md`. M2-M6 exist in `docs/plan/01-milestones.md` as
scope prose only — no task numbers assigned yet.

## Process notes that paid off

- `vt-spec-auditor` found 4 real T18 deviations, including **an idempotency
  guard I invented and then mis-attributed to xterm in both a code comment and
  the ledger**. Run it on every escape-sequence diff; do not skip it because the
  code "looks right".
- `cpp-reviewer` then found two unit tests that could not fail. Both now assert
  content rather than shape.
- `docs-verifier` returned verbatim xterm source for DECOM/1049 semantics. Every
  T19 width expectation was MEASURED with a throwaway probe against the actual
  installed utf8proc rather than recalled — which is how the Thai
  two-cluster result and the `charwidth_ambiguous` trap were found before they
  became bugs.
