# STATE

Phase: **M1 in progress** — T17-T20 done. **Next task: T21 (scrollback).**

## Now

Branch `t20-reflow`, PR open against `main`. T20 is complete and every gate is
green (table below). T17, T18, T19 are merged; T20 is the fourth M1 task.

**Next task: T21** (`docs/plan/02-m0-tasks.md` §M1) — scrollback: ring of
logical lines + per-tab cap + viewport, damage integration. Start at "T21
starts here" below; T20 deliberately left it two things.

## What T20 landed

Reflow, and the cluster storage decision T19 and STATE.md had left open.

**The Cell storage decision is made: tagged `char32_t` + an interned pool.**
Unicode stops at U+10FFFF, so char32_t's top bits are unreachable by any legal
codepoint. `kWideTrailing` marks the right half of a 2-column cluster;
`kClusterTag` indexes a per-grid `ClusterPool`. **Cell stays 32 bytes**, so the
T17 ponytail note in `cell.h` is still achievable — T21 can pack `Color` and
land Cell at 20. The inline-array alternative would have grown Cell to 40 and
fought that directly. Single-codepoint cells are stored literally and never
touch the pool, so an ASCII screen allocates nothing.

`putChar` now stores CLUSTERS. It still takes one codepoint — that is what the
parser emits — but a streaming `ClusterBreaker` (new in `width.h`; same utf8proc
engine as `ClusterIterator`, fed pair by pair so GB12/13 stay armed) decides
whether a codepoint extends the cell under the cursor or starts a new one.
Consequences: a codepoint can advance the cursor 0, 1 or 2 columns, and a
continuation can widen its own cluster (VS16, the second regional indicator).

Rather than reset the break state at every site that moves the cursor, putChar
notices the cursor is not where it left it. **That is necessary but NOT
sufficient — see the review findings below.**

Reflow rules, both in `reflow.h` and both worth reading before touching it:
only the UNWRITTEN tail is trimmed (Krait can tell `ch == 0` from a printed
0x20, so `echo "hi   "` keeps its spaces); a wide pair is never split, detected
via its `kWideTrailing` cell rather than re-measuring width, so there is one
source of truth. The alternate screen is never rewrapped.

## Four bugs, three of them found by review rather than by tests

1. **Blank rows must absorb a narrowing rewrap before anything retires off the
   top.** Without it, shrinking a window scrolls the prompt into history instead
   of wrapping it. This caused 3 of the 4 initial test failures.
2. **ED/EL rewrite cells without moving the cursor**, so putChar's position
   check could not see them and a following combining mark resurrected an erased
   glyph. Fixed by remembering what was last STORED (`m_clusterCh`) and
   re-reading the cell. The "impossible to forget" claim in the first commit was
   wrong; the position check alone is not enough.
3. **A cursor parked on a wide TRAILING half was never placed by reflow** — `k`
   steps by 2 over a pair, so the offset never equalled `k` and fell through to
   the past-content branch.
4. **Reflow cleared row 0's `wrappedFromPrev`**, destroying the wrap point T8
   recorded specifically so T21 could join screen row 0 to the last scrollback
   line. One-line fix, but T21 depends on it.

Also fixed: `ClusterPool` is now non-copyable (its index keys are views into its
own storage, so a copy would dangle), and the M0 spike renderer no longer draws
one CJK character as `??`.

## Evidence, not assertion

Re-run on `t20-reflow` AFTER the review fixes:

| Gate | Result |
|---|---|
| `cmake --build --preset dev` | pass, no warnings beyond the known Qt D9025 noise |
| `ctest --preset dev` | **66/66** (was 47/47) |
| `tests\fuzz\run-smoke.cmd` | pass — 26,336 execs, **0 crashes**, exit 0 |
| clang-tidy, gating set (`bugprone-*`,`concurrency-*`) | clean |
| clang-format | clean |

**The fuzz exec count dropped from 174,174 to ~26,000 and that is expected, not
a regression to hunt.** The target now drives two `resize()` calls per input, and
a rewrap is O(rows*cols) with an allocation per row. An earlier version did
three (including an explicit 1x1) and managed only 22,805; the 1x1 was dropped
because the derived width already reaches 1 column. If exec count matters more
than reflow coverage later, gate the resize on a bit of the input instead.

## T21 starts here

T21 is scrollback: ring of logical lines + per-tab cap + viewport + damage.
T20 left it exactly two things, deliberately:

1. **Scrollback lines are NOT rewrapped.** They keep the width they had when
   they were retired, so after a narrowing resize the ring holds lines wider
   than `cols`. `Grid::resize` only reflows the screen. This is recorded in the
   Reflow row of `docs/conformance.md`. A renderer reading `scrollbackAt()`
   today would read past `cols` — fix it in T21 before anything consumes it.
2. **Pack `Color` into a uint32 and land `Cell` at 20 bytes.** The ponytail note
   in `cell.h` has the full reasoning; T20's encoding was chosen to keep it
   available (zero extra bytes). T21 has the "flood keeps O(cap) memory" test
   that proves the win, which is why it was deferred here.

Related known gap, NOT T21's: ED/EL can erase the lead of a wide cluster and
leave its trailing cell orphaned. Cosmetic, not a safety issue — the fuzz target
asserts the safety invariants and deliberately not this one. It belongs to the
one deferred BCE decision, which is still ONE decision across
ED/EL/IL/DL/SU/SD/1049.

## Watchouts, all of them earned

- **Branch protection is on for `main`.** Commit on a `tN-*` branch and open a
  PR; direct pushes are rejected.
- **Stacked PRs do not auto-retarget.** `gh pr edit N --base main` before merge.
- **Run clang-tidy LOCALLY** — it is installed (LLVM 22.1.0, on PATH) and two
  red CI runs on PR #15 were both lint-only and both reproducible in ~90 s.
  It MUST run inside a VS x64 dev shell; without MSVC's `INCLUDE` every TU
  becomes parse garbage and it reports findings that do NOT reproduce in CI.
  Gating is `.clang-tidy`'s `WarningsAsErrors` (`bugprone-*,concurrency-*`);
  everything else is advisory. Exclude `tests\fuzz\*.cpp` (no
  `compile_commands.json` entry, so it falls back to C++17 and every
  `std::span` becomes a fatal error about nothing).
- **`cmake` is not on the bare PATH.** VS 18 lives at
  `C:\Program Files\Microsoft Visual Studio\18\Community`; CMake at
  `C:\Program Files\CMake\bin`. From Git Bash, `cmd.exe /c` is mangled by MSYS
  path conversion (`/c` becomes `C:\`) — use `cmd.exe //c`.
- **ctest TEST_CASE names must stay ASCII.** An em-dash gets mangled by the
  console codepage until the test cannot be matched by its own name.
- **`if errorlevel 1` in cmd is a SIGNED comparison** and misses -1. Use
  `if %ERRORLEVEL% neq 0`. **pwsh does not throw on a native non-zero exit**
  mid-script; check `$LASTEXITCODE`.
- **`bugprone-misplaced-widening-cast` gates the build.** Widen each operand
  before arithmetic (`static_cast<size_t>(r) + 1`), never the sum.
- The corpus harness only understands `\xNN` escapes — **not** `\r\n`.
- **`run-smoke.cmd` needs `VCPKG_ROOT`** (set inside the VS dev shell). A
  `toolchainFile` change does NOT apply to an already-configured build dir —
  delete `build\fuzz-msvc` if a utf8proc error persists.
- **Re-run every gate AFTER the change that could invalidate it.** T19's first
  CI run died because a fuzz number measured before linking utf8proc was carried
  forward into the PR body.

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
  Deferred deliberately so BCE stays ONE decision.
- `?7` DECAWM and `?25` DECTCEM are accepted but not honored.
- T19 wrote no `tools\gen-width-tables.py`. Deliberate deviation: utf8proc
  2.11.3 ships UCD 17.0.0 already. See `docs/research/t19-width-findings.md`.

## Done so far

T1-T16 = M0, complete and merged (ADR-0013 records the GO verdict).
M1: T17, T18, T19 merged. T20 complete on `t20-reflow`.

Task numbering: **M0 = T1-T16, M1 = T17-T35**, both in
`docs/plan/02-m0-tasks.md`. M2-M6 exist in `docs/plan/01-milestones.md` as
scope prose only — no task numbers assigned yet.

## Process notes that paid off

- **`cpp-reviewer` found the two blocking T20 bugs that 63 passing tests did
  not**, including the ED/EL cluster-resurrection bug, plus four more. Run it on
  every diff before commit; do not skip it because the tests are green.
- `vt-spec-auditor` found 4 real T18 deviations, including an idempotency guard
  invented and then mis-attributed to xterm in both a comment and the ledger.
  **It has NOT yet been run on T20** — reflow has no VT standard to audit
  against, but the wide-cell and cluster behavior is worth its eyes.
- Every T19 width expectation was MEASURED with a throwaway probe against the
  installed utf8proc rather than recalled, which is how the Thai two-cluster
  result and the `charwidth_ambiguous` trap were found before they were bugs.
