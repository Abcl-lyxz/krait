---
name: project-review-patterns
description: Recurring things to check when reviewing Krait VT-core diffs (T5-T21): corpus/fuzz/conformance completeness, Params bounds proofs, Cell/scrollback size budget, viewport-vs-damage coordinate traps, local verify commands
metadata:
  type: project
---

Recurring checks for src/core parser diffs:
- vt-core.md is the strictest rule: every sequence change needs, same commit: corpus cases (valid + malformed + INTERRUPTED-mid-sequence), fuzz seed, conformance.md row. Interrupted variants are the one authors forget (T6 forgot them).
- `Params` (events.h): values uint16 capped 16383 by machine.cpp, count<=32, subparam[] flags for ':'-separated parts. Handlers that ignore subparam flags silently accept malformed colon sequences (xterm ignores colons outside SGR) — check each new CSI handler.
- Private markers (0x3C-0x3F) travel in the `intermediates` span, so `intermediates.empty()` check covers both markers and true intermediates.
- Corpus harness (tests/corpus/harness.cpp): IN lines feed bytes verbatim incl. space; \xNN escapes; per-family TEST_CASE + sink class pattern.
- conformance.md updates so far honest (◐ with explicit gaps listed) — keep verifying, it's rule #5.

**How to apply:** on any src/core/parser diff, grep the new .case files for CAN/SUB/incomplete-input variants before signing off.

T7 confirmations (both recurring patterns hit again):
- handleErase (sgr.cpp) shipped ignoring subparam flags — third reminder that every non-SGR CSI handler must reject colon subparams like handleCsiCursor does. Check this FIRST on any new CSI handler.
- sgr/basic.case shipped without an interrupted-mid-SGR variant (erase.case had one). Authors remember interruption for the second family they write, not the first.
- Session-file hygiene: agent memory once got written under tests/fuzz/seeds/.claude/ — check untracked dirs for stray .claude/ junk before commit.

## T9 confirmation (checklist finally internalized)
- First diff where all three recurring asks shipped unprompted: colon-subparam rejection, interrupted-mid-sequence corpus case (CAN), conformance row honest with explicit gaps. Keep checking, but the author has the pattern now.
- New recurring check for reply-generating handlers: rate/flood guards must have a test that exercises the REFILL path, not just initial-credit exhaustion — test harness wiring (single up-front addInput) can make refill untestable by construction.

## T10 lesson (fuzz tooling)
- Node tooling that re-parses corpus .case files must mirror harness.cpp parseBytes EXACTLY: read as latin1 (not utf8 — c1.case and utf8/basic.case contain raw multibyte, charCodeAt+Buffer masks to low byte), and bound check `i + 3 < len` (mjs shipped `< len + 1`, accepts truncated 1-digit escape). Diff any new byte-parser against harness.cpp:70 line by line.
- Fuzz presets keep asserts live by overriding CMAKE_CXX_FLAGS_RELWITHDEBINFO to "/O2 /Zi" (drops /DNDEBUG). Verified correct; re-check if presets are touched.
- Harness invariant asserts verified sound vs core: intermediates cap 2 (machine.h:44), params uint16<=16383/count<=32, grid cursor always clamped (deferred wrap keeps col<cols). ReplyLimiter credits RESET to 8 per window (never accumulate), so single up-front addInput => max 8 replies/iteration.

## T17 lesson (SGR extended colour) — verifying, not just reading
- Reviews of index-arithmetic diffs are cheap to PROVE, not argue: `Params` invariants (machine.cpp commitParam writes only while count<kMaxParams; every digit clamped to kMaxValue=16383 so the uint16_t cast can never wrap) mean any read bounded by `count-1` or `subEnd-1` is safe. State the bound chain, don't hand-wave.
- Recurring GAP: parameter-cap boundary. Diffs that index near `count` never ship a corpus case at 32 params. Cheapest probe: N leading `1;` params so the introducer's last subparam lands on values[31]. I can add a temp .case, run, and delete — corpus files are auto-discovered by directory_iterator, no CMake edit.
- Attr/Cell size is a real budget: `Line{std::vector<Cell>}` × kMaxScrollback=10'000. Every byte added to `Attr` costs ~2.4 MB per 100 cols of scrollback. Flag size deltas on cell.h diffs; `Color` (kind+index+rgb, 8 B) is the obvious packing target.
- Spike renderer (src/app/terminal_item.cpp) masks colour index with `& 0x0F` and ignores Kind::Rgb — any core colour widening turns "default colour" into "confidently wrong colour" there. Check terminal_item on every cell.h/sgr.cpp diff until T25.
- Local verification (Bash tool): cmake is NOT on PATH — use `/c/Program Files/CMake/bin/cmake.exe`. ASan fuzz binary needs the VC `bin/Hostx64/x64` dir (clang_rt.asan_dynamic-x86_64.dll) prepended to PATH or it exits 127.

## T20 lesson (reflow + cluster storage) — the "self-healing state" trap
- Recurring defect shape: a cached (row,col,pendingWrap) "did the cursor move?" heuristic used INSTEAD of explicit invalidation. It is blind to sequences that move CONTENT without moving the cursor. ED/EL (sgr.cpp clearRange, writes `grid.cellAt(r,c) = Cell{}` directly) are the live ones; ECH/DCH/ICH will be the next. Whenever a diff caches "where I left the cursor", enumerate every handler that writes cells directly — grep `cellAt|lineAt` under src/core/parser.
- Grid's public int members (rows/cols/row/col/pendingWrap/scrollTop) are mutated directly by parser handlers (csi_cursor.cpp, csi_scroll.cpp). Any new Grid private invariant keyed on those fields is unenforceable from inside Grid. Say so.
- reflow.cpp cursor placement: the in-loop `k == cursorOffset` match is skipped for offsets that land INSIDE a wide pair (k advances by 2), and the fallback "past content" branch invents row indices for rows it never emits. Both are cursor-teleport bugs, both bounded by resize()'s final clamp. Check any k-stepping loop that also carries a position.
- terminal_item.cpp (spike renderer) fired AGAIN: cell.h grew kWideTrailing/kClusterTag and the renderer still does `ch >= 0x20 && ch <= 0x7E ? ... : '?'`, so every wide char now paints "??" . Standing rule: any cell.h `ch` encoding change must touch terminal_item.cpp:136 in the same commit.
- Verified-safe patterns worth not re-litigating: `std::deque<std::u32string>` + `unordered_map<u32string_view,...>` keyed into it is sound (deque insertion never invalidates references; rehash moves nodes not data). Only COPYING the pool breaks it — ask for `= delete` on copy ops.

## T21 lesson (scrollback ring + viewport) — bounds that are only half-bounds
- Recurring shape: a "tail" accessor whose cost is stated per ITEM but paid per BYTE. `Scrollback::tailRows(cols, count)` takes the last `count` LOGICAL lines — one of which is capped only by m_maxCells (4M). Whenever a diff bounds work by a count of variable-length objects, restate the bound in cells/bytes and check the worst case a remote stream can build.
- `std::vector::resize()` down NEVER frees capacity. Any "memory cap" counted in `size()` (trim-on-finish, clamp-oversized-line) undercounts resident memory by the retired row width x line cap. Ask for `shrink_to_fit()` at every trim site before accepting a byte-budget claim.
- Viewport offsets have a UNIT trap: `m_viewOffset` is VISUAL rows, `Scrollback::lineCount()` is LOGICAL lines. Anything comparing the two (maxViewOffset) or clamping one by `rows` (viewportRows) silently caps how far history can be reached. Tests that assert only `viewportRows().size() == rows` cannot see it — demand a CONTENT assertion at an offset > rows.
- `DamageList` is in SCREEN row coordinates; anything returning a composed viewport is in VIEWPORT coordinates. They differ by the history rows prepended. Flag any consumer that mixes them.
- Cross-buffer scrollback feeds: `Grid::resize` pushes rows from BOTH buffers (active first, then the inactive one when m_onAlt) into the same ring. Any ring logic that trusts `wrappedFromPrev` to mean "continues the previous push" gets welded across buffers there. grid.cpp resize is the only site; check it whenever push() semantics change.
- IL/DL (csi_scroll.cpp) temporarily set `grid.scrollTop = grid.row`, so `capturesScrollback()` is TRUE for a DL issued at row 0 — DL at the top of the screen retires lines into history. Remember this when reasoning about "which rows reach the ring".
- Packed-bitfield migrations (Color 8->4 B) are cheap to verify: check the kind byte is inside the compared bits (defaulted `operator==` on the packed word gets this right), and that the new payload MASK cannot lose a value a writer could produce (sgr.cpp validates 0-255 via isByte before rgbOf, so the 24-bit mask is behaviour-preserving).
- terminal_item.cpp was updated correctly this time (kind()/index()) — the standing rule keeps paying.

## T8 lesson
- docs/conformance.md rows go stale when stub behavior becomes real (LF "no scroll until T8" row survived T8). Grep conformance.md for the touched controls every grid/parser diff.
- Grid behavior changes tend to ship unit tests only; vt-core rule also wants corpus cases (parser-path: wrap at margin, LF-at-bottom scroll, pendingWrap cancel) in the same commit.

## T23 lesson (shaper pool + shaped-run cache) — bounds in the wrong unit, again
- Third time now (T21 tailRows, T21 resize-capacity, T23 kMaxCacheableText): a
  cap is written in the unit the author *thinks* in, not the unit the data grows
  in. Here the cache bound is CODEPOINTS per run (256) while a run holds up to
  `cols x Grid::kMaxClusterLen` = 240x16 = 3840 codepoints, so ordinary Thai on a
  wide terminal falls off the cache entirely. Standing check: for every new cap,
  compute the worst case from the *storage* invariants (Cell/ClusterPool/Line),
  not from "typical" text. An aggregate byte budget + evict-until-under beats two
  independent per-entry/count caps every time.
- New recurring shape: **a degraded result gets cached as if it were correct.**
  shape_pool.cpp caches an empty ShapedRun when the lazy per-worker loadFace
  fails, so a transient FT failure makes that text invisible for the rest of the
  session AND clears `missingGlyphs`, which is exactly the flag the fallback
  chain triggers on. Whenever a diff caches the output of a fallible operation,
  ask what gets stored on the failure path.
- Worker-pool checklist that paid off here: (1) is the task queue capped, (2)
  does the destructor DRAIN or DROP the backlog (drain = unbounded UI-thread
  block at shutdown), (3) does the loop honour the jthread stop_token or only a
  bool (bool-only = constructor-throw joins forever), (4) is the blocking
  timeout smaller than a frame.
- Verified-correct patterns here, do not re-litigate: `shared_ptr<Batch>`
  captured by value in the task + all result writes under the same mutex as the
  caller's copy-out (a timed-out caller genuinely cannot be written into);
  members ordered so `std::vector<jthread>` is declared LAST hence destroyed
  FIRST, joining before the cache/queue it touches; nested `m_mutex` take in the
  lazy-loadFace block released before the outer take (no recursion, no lock held
  across hb_shape); `upper_bound(offset)-1` cluster mapping cannot underflow
  because clusters[0].offset is always 0 (and is order-independent, so RTL
  visual-order output maps correctly); deleted copy ctor + user dtor suppress
  the implicit move, so `Shaper`'s raw FT/hb handles cannot be stolen.
