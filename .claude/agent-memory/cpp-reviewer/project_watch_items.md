---
name: project-watch-items
description: Deferred/latent issues spotted in past reviews of the Krait terminal, to re-check when the relevant code changes
metadata:
  type: project
---

Watch items from past reviews (verify still true before flagging):

- T3 (M0 skeleton, reviewed clean 2026-07-29):
  - `src/core/CMakeLists.txt` exposes `src/` as PUBLIC include dir. Fine while
    core is alone; once `src/net`/`src/render` exist, linking krait-core grants
    include access to sibling trees — re-check hygiene at T11.
  - Version string "0.0.1" duplicated in root `project(VERSION)`,
    `version.cpp`, and smoke test. Drift risk if versioning becomes real.
  - `tests/core-standalone` builds with NO vcpkg toolchain and duplicates the
    root compile-flag line. It will break the moment utf8proc is linked into
    krait-core, and silently diverges if root flags change. Expect a fix
    when the parser lands.

- T4 (UTF-8 decoder + corpus harness, reviewed clean 2026-07-29):
  - `tests/corpus/harness.cpp` silently skips directive lines that don't match
    `IN `/`EXPECT ` exactly (a typo'd `EXPCT` drops a case with no failure),
    and `stoi`/`stoul` throw on non-hex in `\xNN`/`U+` specs. Acceptable while
    corpus files are repo-owned; re-check if the harness grows formats or
    parses generated/external corpora.
  - Utf8Decoder verified against WHATWG by hand (boundary table, restore loop,
    max-2-outputs bound, finish semantics) — future edits to it should re-run
    the same hand-checks: \xC0\xAF, \xED\xA0\x80, \xF4\x90\x80\x80, \xE0A.

- T5 (DEC ANSI parser machine, reviewed 2026-07-29, hand-traced against
  vt100.net + corpus, no blocking findings):
  - `oscEnd()`/`dcsUnhook()` fire identically for clean termination (ST/BEL)
    and CAN/SUB/C1 abort — sinks cannot tell a truncated OSC 52/DCS payload
    from a complete one. Flagged; if not fixed with an `aborted` flag by T6/T9
    (OSC/DCS consumers), re-flag harder — clipboard/title sinks must not act
    on aborted strings as if complete (xterm discards on CAN).
  - Parser holds no OSC/DCS buffer, so the vt-core "payload caps" obligation
    moved to every ParserEvents sink. Check each new sink (T6+) caps its
    accumulation; events.h was asked to document this contract.
  - `feedByte`'s same-state shortcut skips entry actions; only safe because
    no table row stays in Escape/CsiEntry/DcsEntry while collecting. If a
    future table edit adds a stay-with-action entry to a state that has an
    entry action, this silently breaks — re-trace on any tables.h change.
  - Harness grew a token grammar + `MODE c1` directive (sticky for rest of
    file); T4's silent-skip-of-typo'd-directives risk still present and still
    accepted (repo-owned corpus, assertion counts catch dropped cases).

**Why:** these were deliberately accepted as fine-for-now in a skeleton commit; they become defects only when later milestones touch them.
**How to apply:** when a review touches src/core deps, root flags, or T11 targets, check these first.

## T9 caps/reports (t8-grid, uncommitted at review)
- ReplyLimiter refill coverage: RESOLVED in committed T9 — harness reports test now feeds 64-byte chunks with addInput per chunk (harness.cpp ~385). Contract (addInput before each feed chunk) is documented in caps.h. Fuzz harness deliberately uses single up-front addInput => max 8 replies/iteration.
- handleReport DA1 accepts `CSI 0;1c` (only values[0] checked, count>1 not rejected). Harmless reply, noted not blocked.
- Unreachable Capabilities flags (columns132..ansiColor) + VT220 `?62` branch in da1Reply are dead until M1 — accepted because vt-core rule mandates table-generated DA. Verify flags flip only alongside real implementations.

## T10 fuzz (t9-caps, uncommitted at review)
- extract-seeds.mjs flagged: utf8-read corrupts non-ASCII IN payloads (needs latin1) + escape bound off-by-one vs harness. Verify fixed before trusting corpus-derived seeds.
- Whole clang-cl path (fuzz preset, /clang:-std=c++23, -fsanitize=fuzzer-no-link) is UNVERIFIED — clang-cl not installed on this machine; only fuzz-msvc ran. Re-check the first time someone actually configures the clang preset; also note clang branch drops /WX under a comment claiming warnings-as-errors.
- run-smoke.cmd hard-codes VS18 Community vcvars64 path — breaks on CI/other machines; fine as personal dev script, revisit when CI lands.

## T11 render spike (t10-fuzz, uncommitted at review)
- triangle_item.cpp ignores every failure path (loadShader -> invalid QShader, vbuf/srb/pipeline create() bools) and render() draws unconditionally. Accepted for the spike (baked qrc resources, T12 replaces it); T12 glyph renderer MUST have real device-lost/create-failure handling per render.md — do not let this pattern get copy-pasted.
- Pipeline rebuild only keys on rhi() change; sampleCount/renderPassDescriptor changes not handled. Cannot happen in the spike; re-check if T12 keeps the initialize() skeleton.
- CMakePresets.json dev preset hard-codes C:/Qt/6.10.3/msvc2022_64 (machine-local; pin is 6.11.1, aqt fetch broken). Breaks other machines/CI — expect flip or move to CMakeUserPresets when 6.11.1 lands.
- src/render has no own CMakeLists; spike sources compile inside krait-app's qml module. Root CMakeLists comment "T11 adds add_subdirectory(src/render)" now stale. Real src/render target due at T12.
- Main.qml hex color literals + non-tr() window title accepted for spike only (ui.md bans both) — must not survive into the real shell.
- app/CMakeLists: find_package floor 6.7 vs qt_standard_project_setup(REQUIRES 6.8) — effective floor 6.8, flagged as confusing.

## T12 glyph grid spike (t11-qt-shell, uncommitted at review, no blockers)
- glyph_atlas.cpp:57 assumes FT_PIXEL_MODE_GRAY + positive pitch (OOB read on
  mono/bitmap-strike glyphs). Flagged with one-line guard; verify fixed if any
  font path beyond the two hardcoded outline fonts appears (T13+ shaper atlas).
- grid_item.cpp: pipeline create() failure => per-frame full resource rebuild
  (texture+upload+buffers each frame); texture/sampler/buffer/srb create()
  bools still unchecked (partial improvement over T11 triangle). Real
  device-lost handling per render.md still owed by the real renderer.
- Still latent from T11: pipeline rebuild keys only on rhi() change
  (sampleCount/format ignored); src/render still has no own CMake target
  (promised at T12, not delivered); hardcoded C:/Windows/Fonts paths + 24px.
- Verified-correct patterns worth reusing: own-cell clip handles negative
  bitmap_left; FT_Done on all exits; tight-pack copy for 4-byte-aligned QImage
  scanlines before R8 upload; synchronize-copy of COW QImage for render-thread
  safety; lazy ensureResources because first initialize() precedes first
  synchronize().

## T13 flood bench (t12-atlas-spike, uncommitted at review, no blockers)
- Bench hang path: if pipeline create fails (m_failed) or atlas invalid, the
  !m_pipeline early-return skips the bench branch AND update(), so KRAIT_BENCH
  runs hang forever (flood-report.cmd stalls unattended). Accepted for spike;
  add a watchdog if the bench ever runs in CI.
- finishBench: KRAIT_BENCH_OUT open failure is silent, exit code stays 0, and
  flood-report.cmd `type`s the stale json from a previous run. Re-check when
  perf-auditor starts consuming these files.
- Verified-correct patterns worth reusing: renderer->item queued invokeMethod
  is safe (m_item borrowed in synchronize, reportBench only from render(),
  QQuickRhiItem tears down renderer before item; queued event dropped if
  receiver dies first); hidden-window-then-setGraphicsConfiguration-then-show
  ordering for pre-scenegraph config; resource batch handed to beginPass;
  warmup arithmetic exact (frames 0..59 warm, frame kWarmup starts timer,
  exactly N samples); QString::asprintf %f is locale-independent -> valid JSON.
- lastCompletedGpuTime repeats the same completed-frame value across frames
  (multi-frame latency), so gpu_samples overcounts distinct measurements;
  dev-GPU cpu numbers are vsync-paced (fps == display Hz). Both disclosed in
  m0-spike.json notes — keep that disclosure if baselines are regenerated.

## T8 grid (t7-sgr, uncommitted at review)
- Grid::resize/ctor accept <=0 dims -> row/col=-1 -> cellAt UB. Flagged BLOCKING; verify fix landed before app-layer wires window resize.
- wrappedFromPrev never cleared by ED/EL full-row erase -> stale wrap flags feed M1 reflow. Re-check when reflow lands.
- scrollUp allocates a fresh Line per scrolled row (emplace_back) even when scrollback cap discards one -> recycle candidate when parser bench lands.

## T23 shaper (t23-shaper, uncommitted at review — 3 BLOCKING)
- Blocked on: (1) shape_pool.cpp:208 caches an empty ShapedRun when the worker's
  lazy loadFace fails; (2) kMaxCacheableText=256 codepoints is below a real Thai/
  emoji row (cols x 16); (3) unbounded m_tasks + destructor drains the whole
  backlog. Verify all three fixed before T24 builds on this.
- Accepted latent, re-check at T24/T25:
  - Cache key omits `run.script`/`run.rightToLeft`. Safe ONLY because splitRow
    derives both from the text (first strong codepoint). T24 fallback or any
    splitter change breaks that invariant silently — ask for the fields in the
    key then.
  - `run.shaping` is in the cache key but `Shaper::shape` ignores it, so bold/
    italic currently shape with the regular face. T24 owns face selection.
  - run_splitter.cpp:121 narrows `cps.size()` to uint8_t; only Grid::kMaxClusterLen
    (16) keeps it safe and ClusterPool itself has no length cap. Add a bound on
    ClusterPool if any non-Grid caller ever interns.
  - `splitRow` does not flush on a leading `kWideTrailing` cell and its
    `cps.empty()` (dangling cluster ref) branch has no test.
  - Default `shapeAll` timeout is 250 ms on the CALLING thread vs render.md's
    <10 ms renderer latency budget — T25 must pass a sub-frame timeout.
  - No intra-batch dedupe: N identical rows in one frame are N misses and N
    hb_shape calls.
  - Tests hard-require C:/Windows/Fonts (tahoma/consola); they FAIL rather than
    skip on a host without them.
- src/render finally has its own CMake target (`krait-shaper`, Qt-free), which
  closes the T11/T12 watch item. Spike sources still live in src/render/spike/
  and are compiled by krait-app, so no collision. `target_include_directories
  (krait-shaper PUBLIC src/)` repeats the T3 include-hygiene item.

## T26 device robustness (t26-device, staged at review — 2 BLOCKING)
- Blocked on: (1) terminal_item.cpp itemChange(ItemSceneChange) -> updateGrid()
  -> ensureStarted() runs at width()==0, so ConPTY is created 2x2 and the shell
  banner wraps at 2 columns; (2) gpu_resources.cpp sync() clears
  m_atlasNeedsUpload after a clamp-truncated upload, and synchronize()'s
  handover condition can leave m_atlasPixels shorter than the new texture ->
  permanently blank atlas rows. Verify both before T27 builds on this.
- Accepted latent / re-check later:
  - bufferWidth()/bufferHeight() re-derive QQuickRhiItem::effectiveColorBufferSize().
    Divergence from Qt's own rounding shows up only on fractional item sizes at
    125/150% scaling. Re-flag if any DPI bug report mentions blur.
  - The fake-lost harness exercises a branch production never takes (Qt destroys
    the renderer on scene-graph invalidation). render.md's "device-lost tested"
    is only nominally satisfied.
  - applyDevicePixelRatio's px-unchanged early-out skips updateGrid() after
    already writing m_dpr.
  - tools/dpi-check.cmd tests startup scale, not the mid-session DPI change
    render.md actually names; not wired into ctest or CI.
  - The two changed atlas-upload branches (|| newTexture, the rowsHeld clamp)
    have no test — gpu_resources_test.cpp only ever passes atlasGrew=true with
    an exactly-matching pixel buffer.
  - main.cpp includes <windows.h> without WIN32_LEAN_AND_MEAN/NOMINMAX, unlike
    fontdb.cpp/shape_pool.cpp. T26 added the two defines but placed them AFTER
    `#include "terminal_item.h"`, which pulls conpty_backend.h -> <windows.h>
    unguarded, so both defines are dead and the comment is wrong.

## T26-T35 M1 second half (t26-device, reviewed 2026-07-30 — 3 BLOCKING)
- Blocked on: (1) terminal_item.cpp:777 appendComposition's `clearDirty()` runs
  before rebuildFrame reads dirtyTop/Bottom at 451-452 -> with an IME
  composition active NO glyph rasterised that frame is uploaded; (2) no cap on
  clipboard size before preparePaste (net.md) — 5 copies + 8 regexes on the UI
  thread; (3) Banner.qml detail Text is AutoText, so hostile clipboard renders
  as rich text (banner spoofing).
- Accepted latent / re-check later:
  - frame_builder cached UVs vs atlas growth (see [[project-render-qt-patterns]]
    shapes 6 and 7). PRE-EXISTING from T25, not introduced here.
  - gpu_resources.cpp checks pipeline create() but NOT sampler/buffer/SRB
    create() — the T11/T12 "real device-lost handling" debt is still partial.
  - applySettings compares the CONFIGURED font family against the RESOLVED
    m_family, so an uninstalled/empty family makes every hot reload tear down
    and re-rasterise the whole font stack.
  - Registry::reload() on a parse failure resets every setting to default with
    only a qWarning; `previous` is already in hand and could be restored.
  - Banner.qml Enter accepts a risky paste — the reflex keystroke after
    Ctrl+Shift+V confirms a `sudo rm -rf`.
  - paste.cpp's `[201~` -> `[201 ~` rewrite corrupts legitimate text and does
    not set `sanitised`. ESC is already stripped so it is pure belt-and-braces.
  - Verified CORRECT, do not re-flag: X10 mouse byte range (max 125) and the
    col/row>223 drop; csi_mode/caps shared MouseTracking enum matches xterm and
    DECRQM stays honest (1005/1015 answer 0); ShapePool::shapeAll always does
    `out.assign(runs.size(), {})` so a timeout cannot produce a short span;
    toml::table is std::map-backed so `save()`'s cached `target` pointer is
    stable across later inserts.
- Working-tree drift at review: paste.cpp + registry.cpp + paste_test.cpp had
  UNCOMMITTED fixes (U+2028/2029/NEL normalisation, C1 strip, m_debounce
  delete) not in HEAD. Re-check `git status` before trusting a branch diff.

## T36-T49 M2 backend seam (t36-backend-seam, reviewed 2026-07-31 — 3 BLOCKING)
- Blocked on: (1) ssh_backend.cpp verifyHostKey emits hostKeyPrompt WITHOUT
  armAnswer() (only askForSecret calls it) -> m_answered/m_hostKeyTrusted
  survive a connect cycle, so on reconnect the TOFU gate resolves from the
  previous cycle's answer; (2) search.cpp builds std::regex inside try/catch but
  runs sregex_iterator OUTSIDE it — MSVC throws regex_error(error_complexity)
  during MATCHING, so a backtracking pattern escapes src/core; (3) Vault has no
  mutex yet ssh_backend calls retrieve/store/save from the WORKER thread with
  `Vault*` borrowed from main() — two SSH tabs = m_entries reallocation under a
  live `it->blob.data()`.
- Accepted latent / re-check later:
  - SshBackend::stop() joins the worker with no bound; m_shutdown does not
    interrupt ssh_connect / ssh_userauth_agent / ssh_pki_import_privkey_file.
    net.md's "every network wait has a timeout" is only met for the CV waits.
  - run()'s `attempt` counter never resets after a SUCCESSFUL reconnect, so a
    long-lived flaky session gives up after N lifetime drops, not N consecutive.
  - m_credential is never cleared by armAnswer()/stop(); an answer arriving
    after the 5-min prompt timeout parks plaintext for the object's lifetime.
  - pump()'s write loop tests `n == SSH_ERROR` not `n < 0`; SSH_AGAIN (-2) would
    drive `written` negative into OOB pointer arithmetic (unreachable while the
    session stays blocking, one-char fix).
  - Vault::save() is truncate-in-place, not write-temp-then-rename.
  - handleKittyKeys ignores Params::subparam (DECRQM precedent rejects colon
    subparams); putty_import breaks the WHOLE RegEnumKeyExA loop on
    ERROR_MORE_DATA, dropping every later session.
  - cli.cpp parseTarget turns "[]:22" into host "[]" rather than rejecting.
- Verified CORRECT, do not re-flag: OSC 8 KiB payload cap + 4 KiB decoded
  clipboard cap with the pre-multiply bound check; OSC 52 read gate default-off
  and SILENT; kitty kSupported masking on the way in so the query cannot lie;
  vault.dat parser bounds keyLen/blobLen/entry-count before every allocate;
  Secret move ctor/assign leave no plaintext behind; randomart appendBorder
  truncation arithmetic; hostkey hash read before ssh_key_free; smartSelect
  never splits a UTF-8 sequence; TOML_EXCEPTIONS=0 is set for krait-session.
- SshBackend/Vault are NOT wired into the app yet (TerminalItem still news
  ConptyBackend); the factory is T45. Thread/UI findings are latent until then.
