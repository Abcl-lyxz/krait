---
name: m4-audit-findings
description: M4 (T64-T74) OSC 133 + OSC 9;4 audit 2026-08-01 — setCommandExit anchors the walk at the CURSOR (wrong-prompt write), k= fail-closed deviation, 9;4 has no observing corpus case
metadata:
  type: project
---

Audited `git diff 8627724..HEAD` on branch `t64-m4-power-tools`, 2026-08-01.
Read-only audit; nothing built or run (no vcvars in this shell).

**The bug worth remembering: `Grid::setCommandExit` anchors its backwards walk
at the CURSOR** (`at = lineCount + row + 1`, grid.cpp ~359). If anything moved
the cursor ABOVE the open prompt between `A` and `D` — `CSI H`, a transient
prompt redraw — `prevPrompt` starts below the mark it wants, never sees it, and
writes the status onto the PREVIOUS command's prompt instead. Repro:
`ESC]133;A ST $ one CRLF ESC]133;D;0 ST ESC]133;A ST $ two CRLF ESC[H
ESC]133;D;5 ST` -> exit 5 lands on line 0, clobbering the 0 that was there.
One-line fix: anchor at `absoluteLineCount()`. Verified this does not change
any existing corpus/unit expectation (the cursor is at/below the prompt in all
of them). Residual after that fix: if the marked line is DESTROYED without its
mark being cleared (scroll-region scroll, which discards Lines when
`scrollTop != 0`), a `D` can still land on an older prompt sitting at the floor.

**Mark-lifetime design is otherwise sound and worth not re-auditing:** marks are
fields on `Line` (line.h), there is NO absolute-index-keyed storage anywhere,
and the single cross-line reference (`Grid::m_openPrompt`) is a monotone
`linesEverStarted()` value, not an index. Eviction/reflow/2J/ED-EL/alt-swap/
region-scroll all carry or drop the mark WITH the Line. `reflow.cpp`'s
`for (r = first; r < last)` is correct — `last` is exclusive there.

**Two sequences the audit could not test because they are not implemented:**
`CSI 3J` is an explicit no-op in `handleErase` ("no-op until the real grid"),
and `Session::escDispatch` implements NO ESC finals at all, so DECALN (`ESC # 8`)
does nothing. Neither can strand a mark, but neither can be corpus-tested.

**T67's `reports/` corpus trap is now HALF closed.** `parser_fuzz.cpp` routes
OSC through a real `OscHandler` + `Grid` (good), and the new `shell/` corpus
category runs a real `Session` and reads marks back. But `harness.cpp`'s
`CursorSink` still no-ops OSC, and `describeMarks()` reports only marks — so
**OSC 9;4 still has zero corpus coverage that could fail if `parseProgress`
were deleted**. Its grammar is pinned by unit tests only. The conformance row
says this out loud, which is honest but is still short of rules/vt-core.md.

**Spec deviation to keep flagging:** `isSecondaryPrompt` (osc.cpp ~66) returns
`field != "k=i"` for ANY `k=`, so `k=`, `k=x`, `k=initial` drop the prompt mark
entirely. ghostty's parser states the spec rule verbatim — ignore unknown or
malformed options — and maps an unknown kind to null (= unspecified = still a
prompt). Fail-closed here means a shell that learns a new kind letter loses
every jump target.

`docs/conformance.md`'s OSC row is the most complete row in the file and
self-reports its own evidence gaps. Do not re-audit it for honesty; audit it
for coverage.
