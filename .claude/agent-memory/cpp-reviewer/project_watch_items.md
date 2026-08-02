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

## T52 backend factory + UI wiring (t52-backend-factory, uncommitted, reviewed 2026-07-31 — 3 BLOCKING)
- Blocked on: (1) terminal_item.cpp resetSession() assumes `disconnect(this)`
  cancels in-flight queued emissions — true for ConPTY (posts invokeMethod to
  ITSELF) but FALSE for SshBackend (emits from the worker, so the QMetaCallEvent
  is posted to the TerminalItem) → old host's bytes/prompts land in the NEXT
  session; (2) verifyHostKey emits hostKeyPrompt then fail() back-to-back, so
  Main.qml onErrorRaised overwrites the Changed-key "danger" banner (fingerprint
  + randomart lost) in the same event-loop turn; (3) m_started is true with
  m_backend null in the KRAIT_TERM_BENCH path → any keystroke null-derefs at
  terminal_item.cpp:821 (also 683/907/992/303).
- Accepted latent / re-check later:
  - QML banner is a mode machine; only dismiss() calls endInput(). onErrorRaised
    / onConnectionNotice / onPasteConfirmRequested leave a typed password in the
    TextInput and leave mode=="credential".
  - banner.message takes server text raw: empty kbdint prompt → `visible:
    message.length > 0` false → invisible banner + 5-min silent auth hang; 36
    sanitised lines → TerminalView height goes negative.
  - SshBackend::stop() (unbounded join) now runs on the GUI thread on every
    palette session switch, not just at exit. ConPTY adds a 500 ms
    WaitForSingleObject.
  - Every `krait <profile>` launch spawns a default PowerShell during
    engine.loadFromModule and main() immediately tears it down.
  - splitCommand/expandEnv (quote parsing, two-shot buffer growth) are in an
    anonymous namespace with zero tests; backend_factory_test covers only the
    enum/port mapping.
  - The five new SSH connect() calls are AutoConnection, not the explicit
    Qt::QueuedConnection cpp.md demands.
- Verified CORRECT, do not re-flag: main.cpp declaration order (app → registry →
  vault → engine) genuinely outlives the backends, ~QQmlApplicationEngine
  deletes root objects first; describeHostKey askable=false for Changed/
  OtherType/Error and Main.qml hides the accept button (showAccept gates both
  the button and handleConfirmKey); Vault now HAS its mutex; armAnswer()/stop()
  now clear m_credential; ExpandEnvironmentStringsW `written <= buffer.size()`
  and SearchPathW `length >= MAX_PATH` comparisons match the documented
  count-includes-null semantics; function-local `static const bool safeSearch`
  init is thread-safe (magic statics); deleteLater on a parented child is fine;
  respondCredential wipes its QByteArray with SecureZeroMemory.

## T54 telnet backend (reviewed 2026-07-31, branch t54-backend-factory)

- BLOCKING: `TelnetBackend::stop()` runs on a QThreadPool thread via
  `TerminalItem::resetSession()` — see thread pattern 13.
- BLOCKING: no write backpressure. `flushReply()`/`writeInput()` call
  `QTcpSocket::write` unconditionally; Qt's write buffer has no cap
  (`setReadBufferSize` caps reads only, there is no write equivalent). A server
  that agrees TERMINAL-TYPE then streams `IAC SB 24 01 IAC SE` and stops
  reading gets a 3.3x amplifier (6 bytes in, 20 out) into an unbounded buffer.
  net.md wants both a cap and answerback rate-limiting.
- HIGH: initial NAWS reports 80x24, always. `openSocket()` copies
  `m_config.settings` into the Negotiator BEFORE `handleConnected()` writes
  m_cols/m_rows into it. Only a later window resize corrects it. Contract test
  uses 80x24 so it cannot see this.
- MED: the connect timer only calls `fail()`; it never aborts the socket. With
  the production default maxReconnectAttempts=0 the stale connect survives and
  can emit `connected()` after the connect-failed banner, or error again for a
  second banner (m_sawError is not set on the timer path).
- MED: `TelnetConfig::maxReconnectAttempts` defaults to 0 and
  `backend_factory.cpp` never sets it (SshConfig defaults to 5) — retry timer,
  m_attempt, scheduleReconnect and the `reconnecting` signal are unreachable
  outside the contract test.
- Fuzz honesty: `telnet_fuzz.cpp` never asserts anything about `data` (its own
  header comment names that as THE invariant), and it feeds at chunk 1/3/whole
  but discards all three results instead of comparing them. It also never calls
  `start()`, so the WANTYES rows of RFC 1143 are unfuzzed. The reply walker is
  not IAC-doubling-aware (latent false alarm if resize's range widens past
  cols=0xFF00).
- `run-smoke.cmd` passes `tests\fuzz\seeds-telnet` as libFuzzer's writable
  corpus dir, so the committed seed dir grows every run (~200 SHA-named files
  already sitting there next to the 14 hand-written seeds). parser-fuzz avoids
  this by using `build\%PRESET%\corpus`.
- Verified CORRECT, do not re-flag: the RFC 1143 tables in
  receiveWill/Wont/Do/Dont match docs/research/t54-telnet-findings.md row for
  row, INCLUDING the merged WantNoOpposite+WantYesEmpty case in receiveWill
  (both are him=YES per the published table); `m_subData` is bounded on every
  push and the overflow flag is reset on all three exits from SubIac; no
  subnegotiation parameter byte can reach `data`; `encodeInput` doubles 0xFF in
  both NVT and BINARY and there is no path that writes a lone 0xFF;
  `m_negotiator` cannot be null in handleReadyRead/writeInput/handleConnected
  (it is built in openSocket before the socket); openSocket's
  disconnect+abort+deleteLater ordering is safe; the fuzz presets override
  CMAKE_CXX_FLAGS_RELWITHDEBINFO to "/O2 /Zi" with no /DNDEBUG, so the asserts
  really do fire; MSVC C4062 + /WX does make the defaultless switches in
  profile.cpp/backend_factory.cpp break at compile time as their comments claim.

## T60 — agent_bridge (libssh agent client ↔ Windows OpenSSH named pipe), 2026-07-31

- BLOCKING: `SshBackend::stop()` never touches `m_impl->agent`. The worker can
  be parked in `ssh_userauth_agent` → libssh `atomicio` recv on the bridge fd,
  which only returns when the relay replies or `m_relayEnd` is shut down. Relay
  is parked in `ReadFile` on the agent pipe. FIDO/smartcard touch, wedged agent
  service, or a squatted pipe = join forever; `TerminalItem::~TerminalItem`
  does that join on the GUI thread. This is memory item 7 coming true verbatim.
  Fix shape: `AgentBridge::cancel()` (shutdown + CancelIoEx under a mutex shared
  with stop()) called from SshBackend::stop() before the worker join.
- BLOCKING: `CancelIoEx` only marks I/O **outstanding at the moment of the
  call**. `pump()` between `WriteFile` and the reply `ReadFile` has nothing to
  cancel, so the next ReadFile is uncancellable and `join()` is unbounded — the
  code comment claiming otherwise is wrong. Needs an atomic checked before every
  pipe read PLUS a re-issued/bounded cancel (or FILE_FLAG_OVERLAPPED + event).
- MED: the pipe direction is the only genuinely untrusted framing in the file
  (the socket side is our own libssh) and it has zero malformed-input tests —
  reply length 0, > 256 KiB, truncated body, close-after-header. FakeAgent
  already takes an arbitrary reply, so each case is ~4 lines. net.md.
- MED (test): `FakeAgent::serve()` reads the request body with one un-looped
  `ReadFile` and never checks `got == length` → the byte-for-byte request
  assertion can flake on a byte-mode pipe.
- LOW: `openPipe` never checks who owns `\\.\pipe\openssh-ssh-agent`. If the
  agent service is down, any local process can create the name first and feed
  Krait chosen identities/signatures. The loopback side is explicitly hardened
  against this exact class; the pipe side is not. `GetNamedPipeServerProcessId`
  + owner-SID check.
- LOW: we read SSH_AUTH_SOCK with `GetEnvironmentVariableA` (live process env),
  libssh reads it with `getenv` (MSVC CRT snapshot, agent.c:256). A runtime
  `SetEnvironmentVariableA` desynchronizes them and silently kills agent auth.
  `getenv_s`/`_dupenv_s` is the same source libssh reads.
- Verified CORRECT, do not re-flag: `loopbackPair`'s
  `getsockname(dialled) == getpeername(accepted)` check IS sufficient — the
  4-tuple is unique, so the reverse comparison is redundant and an impostor
  cannot duplicate our source port to the same listener port; worst case is a
  DoS that falls through to the next auth rung. All framing arithmetic is
  exact and overflow-free (`size_t(length)+4`, `4-filled`, `length-filled`,
  every DWORD cast bounded by the 256 KiB cap); both pipe read loops handle
  partial reads. Ignoring `ssh_set_agent_socket`'s return is safe: `socket.c`
  `ssh_socket_set_fd` assigns `s->fd` before any failure return, so `ssh_free`
  owns the fd on every path — no leak and no double close, including the
  reconnect cycle (`join()` makes `m_relay` non-joinable, so `start()` re-arms).
  `GetEnvironmentVariableA(.., nullptr, 0) > 1` is the right set-and-non-empty
  test. `ssh_userauth_agent`/`ssh_socket_unix` are NOT `#ifndef _WIN32`-guarded
  in libssh 0.12, so honouring SSH_AUTH_SOCK is not a dead branch.

## T60 re-review + T61/T62 (2026-07-31, second pass)

T60 fixes VERIFIED CORRECT — do not re-derive or re-flag:
- `stop()` joining outside `m_mutex` is safe: `m_relay` is touched only by
  start()/stop() on one thread and `cancel()` never touches it, so the join
  races nothing and the documented cancel-blocks-on-mutex deadlock is real.
- `pipeIo` cannot return with the kernel owning its stack `OVERLAPPED`/buffer:
  all three exits are pre-issue flag check, start-failed-not-PENDING, or
  `CancelIoEx` + blocking `GetOverlappedResult`.
- `m_stopping` reset at the END of `stop()` is right for reconnect (runOnce
  teardown calls `agent.stop()` every cycle).
- The `SO_RCVTIMEO` on the relay socket is measured, not defensive. Leave it.

- BLOCKING (found this pass): `AgentBridge::start()` takes NO lock and does
  `m_stopping.store(false)` before publishing handles, so a `cancel()` landing
  during `openPipe` (up to `kPipeBusyWaitMs`=1000 ms in `WaitNamedPipeW`) is
  erased and the GUI thread hangs in `SshBackend::stop()`'s join again. Pattern
  to check on EVERY start/cancel/stop trio in this repo: the *start* side must
  join the same mutex and must not clear the stop flag.

T61 verified against THIS TREE's libssh 0.12 source
(`build/fuzz-msvc/vcpkg_installed/vcpkg/blds/libssh/src/libssh-0-*/src/`) —
that source tree is the authoritative local answer for libssh questions:
- `ssh_userauth_publickey(session, user, const ssh_key)` never frees or mutates
  the key; reusing it after DENIED is exactly libssh's own `_auto` order.
- `ssh_pki_copy_cert_to_privkey` DEEP-copies into `privkey->cert` and requires
  `ssh_key_cmp(..., SSH_KEY_CMP_PUBLIC)==0`; freeing `cert` right after is
  correct, `ssh_key_free(key)` releases the copy. No leak on any path.
- sk-* refusal IS reachable: pki.c parses `SSH_KEYTYPE_SK_*` unconditionally,
  only signing is `#ifdef WITH_FIDO2`. The two `_CERT01` switch cases are dead
  (a privkey file never imports as a cert type) but harmless.
- `m_authHint` is worker-thread only (written in tryPublicKey, cleared+read in
  authenticate, both inside runOnce). Not a race.

- **libssh expands `~`/`%d` ONLY on paths that pass through
  `ssh_options_apply`** (`SSH_OPT_EXP_FLAG_IDENTITY`, the certificate loop).
  `ssh_pki_import_privkey_file` / `ssh_pki_import_cert_file` `fopen()` the
  string verbatim. Any code path that feeds a config path straight to a
  `ssh_pki_import_*_file` needs its own tilde expansion. T62's ssh_config
  importer makes `~/.ssh/id_ed25519` the common case; the failure is SILENT
  (`SSH_EOF` = -2, not `SSH_ERROR` = -1, so no passphrase prompt, no hint).

T62 verified CORRECT, do not re-flag:
- `defaultSshConfigPath`'s `getenv_s` two-call pattern has no off-by-one
  (`needed` includes the NUL; `string(needed,'\0')`; `resize(needed-1)`).
- mRemoteNG `openNodes` is balanced on every path; `folders`/`inherited` are
  pushed/popped together and cannot desync; malformed XML exits via `atEnd()`
  after the reader errors. Self-closing-sibling regression IS covered by the
  "containers become folders" test.
- No `%N` format-marker injection through `QString::arg`: importer text always
  goes through the multi-arg `arg(QString, QString)` overload, which does not
  re-substitute.
- Open, LOW: no size cap in `SessionModel::readTextFile` (GUI thread,
  `readAll()`), `Block::set` is O(k^2) in distinct keywords, mRemoteNG is
  O(N*depth) in `folders.join()` per leaf. Fixed paths, so hang not privilege.

## T67 — taskbar progress (OSC 9;4) + jump-to-prompt (OSC 133)

BLOCKING found (fix before re-review re-flags):
- `Grid::viewOffsetCeiling() = max(maxViewOffset(), m_viewOffset)` is
  SELF-REFERENTIAL. Using the current offset as its own ceiling makes
  `pushToScrollback`'s `min(off+1, ceiling)` a no-op and turns `scrollView`
  into a one-way ratchet above `maxViewOffset()` (which under-counts, being
  LOGICAL lines vs the visual rows `scrollToLine` computes). Any future
  "sticky deep offset" needs a SEPARATE member, never `m_viewOffset` itself.
- `TaskbarProgress::apply()` reaches `m_window->winId()` synchronously from
  `~TerminalItem` -> `forget()` -> `schedule()`. `winId()` CREATES a platform
  window; after `QWindow::close()` (which per Qt docs "effectively calls
  destroy()") that resurrects a native window mid-teardown. A `QPointer` does
  NOT guard this: QPointer clears at the top of `~QObject`, and QQuickWindow
  deletes its content item (and therefore the TerminalItems) in the
  `~QQuickWindow` BODY, before `~QObject` runs. Guard on
  `QWindow::handle() == nullptr` (public, qwindow.h:229) instead.

T67 verified CORRECT, do not re-flag:
- The throttle cannot drop a final value: `apply()` re-reads `current()`
  rather than a captured snapshot, and `QTimer::singleShot(ms, this, fn)` is
  cancelled with the context object, so `m_pending` stuck true is moot.
- COM: no `CoInitializeEx` is right (Qt OleInitializes the GUI thread STA);
  `IID_PPV_ARGS` works with a header-level `struct ITaskbarList3;` because
  shobjidl.h's uuid'd redeclaration precedes the use in the .cpp; no leak on
  the `HrInit` failure path; `Release()` precedes Qt's OleUninitialize because
  `QGuiApplication` is declared before the taskbar in main().
- `nativeEventFilter` returning false always, and the Explorer-restart
  release/re-acquire, are both correct. `removeNativeEventFilter` in the dtor
  is redundant with `~QAbstractNativeEventFilter` but harmless.
- `m_applied` / `m_buttonReady`: the early return that skips recording
  `m_applied` is deliberate and has no stuck state.
- Parser: `parseExitStatus` is overflow-checked, `parseProgress` clamps to
  0-100, subparam/`k=`/positional-status handling matches ghostty+wezterm,
  and `end(aborted||overflowed)` kills interrupted strings. Corpus has
  CAN/SUB/new-introducer variants; fuzz seeds and the conformance row landed.
- `notify.longCommandSeconds` cannot overflow `*1000`: `Registry::integer`
  returns `int64_t` and out-of-range TOML keeps the default (min 1, max 3600),
  so remote `C`/`D` pairs are bounded to ~1 notification/sec.
- `visualRowsOfLine` counts rows identically to `reflow()` (verified line by
  line) and is bounds-guarded; `scrollToLine`'s INT_MAX clamp is real.

## T68/T69 triggers + snippet bar (src/app/session/triggers.*, terminal_item)

BLOCKING found:
- `feed()` dedupes a straddling match with `begin + length <= tailLen`, which
  only drops matches whose END is inside the carried tail. Any pattern whose
  match GROWS as the line completes (`err(or)?`, `error.*`, `\w+`, `Continue.*`)
  fires twice — duplicate banner/log and a duplicate `send` (burst=3 covers the
  second). Correct shape: dedupe on the ABSOLUTE begin offset per rule, not on
  the end.
- `plainText()` is a stateless per-chunk skipper and diverges from
  `parser/tables.h`, where `t[s][0x1B] = {None, State::Escape}` makes ESC an
  ANYWHERE transition. `ESC ESC ] 0 ; error BEL` and `ESC [ 0 ESC ] 0 ; error
  BEL` both leak the OSC payload as matchable text, and a sequence split across
  two chunks leaks whatever lands in the second. Defeats the "cannot be baited
  from inside an escape payload" claim in triggers.h AND docs/configuration.md.

Latent (re-flag if untouched):
- `feed()` computes the carried tail from the TRUNCATED subject when a chunk
  exceeds kMaxScan, gluing the 64 KB mark to bytes that arrive megabytes later
  — a fabricated adjacency that can fire a rule. Clear m_tail on truncation.
- `catch (regex_error) { continue; }` in feed/highlightRanges retries a rule
  that hit MSVC's complexity ceiling forever. `highlightRanges` runs per VISIBLE
  ROW per `rebuildFrame()`, and rebuildFrame runs per output CHUNK — so the
  ceiling is paid rows x chunks on the UI thread. Disable the rule after the
  first complexity throw.
- `TerminalItem::logTrigger` retries `resolveConfigDir` + `mkpath` + `open` +
  `emit errorRaised` on EVERY hit when the path is unwritable (up to 64/chunk),
  and never reopens when `triggers.logFile` changes.

Verified CORRECT, do not re-flag:
- `lineText`'s `columns` out-param: `charge()` does `resize(out.size(), cell)`
  after each cell, wide-trailing cells contribute nothing, holes charge 1 byte,
  terminator = `last`. size == text.size()+1, so rebuildFrame's
  `end >= columns.size()` guard rejects nothing valid. Map is in step.
- `runTriggers` placement (after `Session::feed`, before `rebuildFrame`) IS
  safe: `Grid::viewportRows()` returns by VALUE, so a banner->strip-height->
  `Grid::resize()` re-entry during the emit cannot dangle m_viewport, and
  rebuildFrame re-reads everything afterwards.
- `takeSendToken` arithmetic: no overflow (steps*interval <= nowMs), refilledMs
  floor-aligns, burst clamps to kSendBurst. Sound token bucket.
- The subject/tail buffers are bounded (4 KB + 64 KB); `sregex_iterator` holds
  iterators into a local `subject` that nothing mutates; handleOutput arrives
  QueuedConnection so the engine is UI-thread-only.
- `sessionTitle()` is profile-derived, NOT remote-settable (OSC title is still
  core silence), so the tab-separated trigger log cannot be forged.
- i18n: EN+TH both landed for every new tr()/qsTr() string.

## T73 — sftp_model shell-integration installer + editor round trip (2026-08-01)

BLOCKING found (3):
- `stopEditing(name)` keys on the LEAF NAME while `m_edits` is keyed by local
  path; two watched files sharing a name (different remote dirs) → the wrong
  Edit is discarded, the clicked one keeps auto-uploading, and the stopped one
  silently stops reaching the server. Fix: key stopEditing on `localPath`
  (the QML row already carries it).
- `launchEditor` with `editor.command` empty (the DEFAULT) calls
  `QDesktopServices::openUrl` on a REMOTE-named temp file → Windows shell
  association runs `.exe/.bat/.lnk/.hta` the server chose. Recurring shape:
  "open with the OS default" on remote-derived content.
- Probe failure ≠ "file absent": every non-ENOENT `sftpGet` failure collapses
  into `found` being empty, and the resulting "Add Krait's block to X?" write
  is an `O_TRUNC` put that erases an existing rc file. `Sftp::put` opens the
  remote O_TRUNC, so a wrong "absent" verdict is unrecoverable data loss.

Non-blocking, still open:
- `confirmShellIntegration` ignores `QFile::write`/close status → a short write
  uploads a truncated rc over a live one. Same shape as the T5x staged-write
  finding; QSaveFile or a write()==size check.
- Probe reads the rc with `readAll()` + `QString::fromUtf8`, no cap, five files
  automatically. Also makes the `int i` loop counters in blockState/spliceBlock
  theoretically overflowable. `Sftp::get` streams (disk-bounded) but the model
  is not.
- `m_editRequests` is never cleared in `attach()` — stale ids accumulate.
- `finishProbe()` passes `m_install.found.constFirst()` by reference into
  `chooseShellTarget`, which can `resetInstall()` (destroys `found`). Safe only
  by statement ordering today.

Verified CORRECT, do not re-flag:
- `handleFinished` `discardEdit(*it)` then `m_edits.erase(it)` — `removePath`
  emits nothing, so `it` is live; `errorRaised` reaches only QML banner setters,
  no re-entry into SftpModel.
- `flushEdits` iterates a `std::exchange`d COPY; re-inserting into `m_editDirty`
  is safe, and `uploading` cannot latch (cancel path clears it before returning).
- `spliceBlock`/`blockState` agree on the span for every 1-begin/1-end input;
  CRLF and no-trailing-newline are preserved and tested. Damaged (end-before-
  begin, doubled block, lone marker) is refused by the caller.
- `m_remotePath` non-empty ⟹ `m_homePath` non-empty, so `afterResolve` cannot
  strand the flow at "probing". `cancel()` → `sftpCancelAll()` emits
  `sftpFinished(cancelled)` for every queued request, so no stage is orphaned.
- Deleting the scratch/temp dir mid-put is NOT a remote-truncation risk:
  `Sftp::put` opens the LOCAL ifstream before `sftp_open(..., O_TRUNC)`.
- `~SftpModel` calling `discardEdit` → `m_watcher->removePath` is fine; QObject
  children die after the derived dtor body.

## T74 — broadcast (src/app/broadcast.*) + quake (src/app/quake.*)

Open at review time (2026-08-01, uncommitted on t64-m4-power-tools):
- BLOCKING: `BroadcastModel::resolve(bool)` is not bound to the line it
  displayed. `m_pending` is a single slot; `confirmRequested` routes to
  `root.currentPane()`. Two tabs → two held dangerous lines → Accept on the
  older banner runs the NEWER line. Fix = pass `banner.detail` (or a token)
  into resolve() and refuse on mismatch.
- BLOCKING: `TerminalItem::sendBroadcast` liveness test is
  `m_session && m_started && m_backend && !m_exited` — it misses
  "connected but RECONNECTING". SSH queues into `m_writeQueue`,
  `TcpBackend::writeInput` DROPS on `!isConnected()`, yet true is returned and
  the line is counted delivered. IBackend has no `isConnected()`; the cheap fix
  is an `m_reconnecting` flag driven by the existing SSH
  `reconnecting`/`connected` lambdas in adoptBackend.
- BLOCKING: `QuakeWindow::applyHotkey()` live-failure path emits `hotkeyFailed`
  → a banner on a window that is HIDDEN in drop-down mode. main() enforces
  "stay visible on failure" only at startup. Fix = showDropDown() from the
  failure paths when `m_dropDown && !m_visible`.
- `attach()` returns before `connect(registry, &Registry::changed, ...)` when
  quake.hotkey is EMPTY (the default) → setting it later is dead until restart,
  contradicting docs/configuration.md ("re-registers immediately — no restart").
- Broadcast confirm only holds `PasteRisk::DangerousCommand`; `Multiline` /
  `ExecutesOnPaste` (which `PasteResult::needsConfirm()` covers) fan out to
  every host unconfirmed.
- No test destroys a marked target QObject without calling `forget()`, so the
  central QPointer-nulls lifetime claim is untested.

Verified CORRECT, do not re-flag:
- `rebuildRows()`'s `std::erase_if(m_targets, ...)` is re-entrancy-safe from
  every caller. `offer()` calls it mid-range-for but `return`s on the next
  statement (dangling ref never read); `fanOut()`/`begin()`/`stop()`/`forget()`
  all finish iterating first. `rebuildRows` emits `tabsChanged` LAST.
- No synchronous re-entry into `m_targets` from inside `fanOut`'s loop: every
  `IBackend::writeInput` queues (conpty/ssh/serial mutex+cv, tcp socket write),
  `sendPaste`→`rebuildFrame` emits nothing QML routes back to offer/forget, and
  `emit sessionChanged()` fires only from `openProfile()`.
- `Component.onDestruction` → `forget(view)`: QPointer is ALREADY null there
  (~QObject zeroes sharedRefcount before the declarative destroyed callback),
  so `target.tab == tab` is false — the `|| target.tab.isNull()` clause is
  load-bearing, not belt-and-braces. Do not "simplify" it away.
- Nothing reaches a pty bypassing `input::preparePaste`; `looksDangerous()` runs
  on the SANITISED text, so classifier and payload agree. The separate
  `sendInput("\r")` outside the bracketed-paste wrapper is correct.
- Quake HWND caching (register-time cache, never `winId()` at unregister),
  `removeNativeEventFilter` in the dtor, and `QuakeWindow quake;` declared
  before `QQmlApplicationEngine engine;` (destroyed after it) are all right.
- `Registry` keeps the DEFAULT for out-of-range values rather than clamping, so
  `seconds * 1000` in `restartIdleTimer` cannot overflow.
- Notifier/TaskbarProgress cache their HWND lazily (first notify / per apply),
  AFTER quake's `setFlags()`, so no stale-HWND cross-feature bug.

## M4 gate audit (branch t64-m4-power-tools, 8627724..HEAD) — 2026-08-01

Open findings handed to the lead auditor (re-check before re-flagging):
- `Sftp::listDir` (sftp.cpp:225) is the ONLY blocking libssh loop in the repo
  with no `m_shutdown` read and no interleave hook. 65536 iterations x the
  15 s `SSH_OPTIONS_TIMEOUT` = an unjoinable worker. `realpath`/`stat` are one
  round trip so they are bounded; `get`/`put` cancel via the Progress callback.
- `interleaveShell()` (ssh_backend.cpp:1116) reads only `is_stderr=0`; pump()
  reads 1 as well at :905 with a comment saying why. Also skips
  `m_forwards.service()` and the `m_resizePending` drain, so tunnels + pty
  resize are starved for the whole transfer.
- `SftpModel::cancelShellIntegration()` resets `m_install` but not `m_open`, so
  an in-flight Probe resumes the chain with `scratchDir` empty and downloads to
  a RELATIVE "probe" path in the process CWD. Cancel is reachable during
  "probing" (FilePanel.qml:627 hides it only for "writing").
- `launchEditor(const QString&, const QString&)` is called with `it->local,
  it->name`; the header comment claims by-value. `QDesktopServices::openUrl` ->
  ShellExecute pumps a nested loop that delivers queued `sftpFinished`.
- Pre-existing: `~TerminalItem` (terminal_item.cpp:713) calls `backend->stop()`
  (which joins) on the GUI thread; `resetSession()` offloads it to the pool.

Verified CORRECT, do not re-flag:
- SFTP handle lifetimes: every sftp_opendir/sftp_open is closed on EVERY exit
  path; every sftp_attributes freed; sftp_limits_free, ssh_string_free_char
  present. `open()` leaving `m_session` set after an `sftp_init` failure is
  deliberate and safe.
- The listDir cap counts ITERATIONS (`++seen` before the isSafeName filter),
  not stored entries. Test at tests/unit/sftp_test.cpp:480.
- Truncated-download detection (sftp.cpp:366) deletes the file and is distinct
  from `cancelled()`. Test at sftp_test.cpp:538.
- Teardown ordering: `m_sftp.close()` (sftp_free) runs FIRST in runOnce's
  teardown lambda, before ssh_channel_free/ssh_disconnect/ssh_free, on both
  exits; member order also destroys `m_sftp` before `m_impl`.
- `queueSftp` closes the TOCTOU: the `m_connected` read and the push are under
  the same mutex teardown clears them under. The `emit` inside that lock is
  safe ONLY because every sftpFinished connection is explicit QueuedConnection
  (sftp_model.cpp:219) — a Direct one would deadlock the non-recursive mutex.
- Cancel epoch: bump+swap under one lock, snapshot under the same lock as the
  pop; request ids are never reused and handleFinished drops unknown ids.
- reflow's mark gather uses the same half-open [first,last) as joinLogicalLine;
  `visualRowsOfLine` reproduces reflow's wrap arithmetic exactly; `prevPrompt`'s
  `i-- > floor` is the correct inclusive form; `indexOfStable` + `clear()`
  advancing `m_dropped` degrade a stale `m_openPrompt` to floor 0.
- Notifier's `wcsncpy_s(..., _TRUNCATE)` sized by ARRAYSIZE bounds remote
  command output into szInfo/szInfoTitle; no format string anywhere on it.

## T84 — renderer paints images + OSC 66 sized text (reviewed 2026-08-02)

Reported (highest first):
1. `terminal_item.cpp` sized atlas: NO `takeGrew() -> m_builder->invalidate()`.
   ~43 distinct scaled glyphs fills the initial 512px height, it doubles,
   every cached row's sized `v` is 2x wrong. Main atlas has the fix; the
   second atlas shipped without it.
2. `synchronize()` `if (m_gpu.hasImage(id)) continue;` — `ImageStore::put`
   replaces an id in place, so a re-transmitted kitty image draws the OLD
   pixels forever. Fix = expose `Entry::sequence` and compare.
3. GPU image set drops only on `store->find(id)==nullptr`; ImageStore evicts by
   BYTES only, so 1x1 images never evict => unbounded QRhiTexture/SRB growth.
4. `Placement::anchor` names only the LOGICAL line, resolved to the FIRST
   viewport row of it — an image emitted on a wrapped continuation row draws
   N rows too high. Ledger documents the "above the viewport" gap, not this one.
5. `setSizedAtlasPixels` called every frame with sized text => 1-8 MiB memcpy +
   full texture upload per frame inside synchronize().
6. `m_sizedAtlas` guarded by `if (== nullptr)`, never reset on font/DPI change.
7. `fontdb.cpp` scaled reshape: `shapeAll(single,...)` per scaled run with a
   fresh 8ms DURATION => frame cost O(scaled runs x 8ms) on the GUI thread.
8. `grid.cpp stableLineOfScreenRow` clamps `last` to `m_screen.size()` and
   loops `r <= last` => OOB at screenRow==rows. Unreachable today (row is
   clamped to rows-1) but it is a public conversion API.
9. `docs/configuration.md` +69 lines of config-sync docs, unrelated to T84.

Verified CORRECT, do not re-flag:
- `viewportTopStable()`'s `m_viewOffset - 1` DOES mirror `viewRows()`: the top
  returned row is at wrapped index `have - fromEnd`, i.e. `fromEnd - 1` visual
  rows before the newest. Checked against scrollback.cpp:124-128.
- `stableAtVisualFromEnd` terminates in <= fromEnd+1 line steps (every logical
  line >= 1 visual row) and its cost is the same order as the `viewRows()` call
  that already happens in the same rebuild — not a new DoS.
- `m_dropped` handling: returns `m_dropped` on empty/cols<1 and on running off
  the oldest line, matching `indexOfStable`'s clamp.
- `stableLineOfScreenRow`'s `--index` guard is correct for the normal path
  (only decrements when scrollback is non-empty AND screen row 0 wraps).
- `appendImages` batching: stable_sort by zIndex, `emplace` keeps the FIRST row
  per stable index, `m_belowBatches` is set exactly once at the z>=0 boundary
  and falls back to `imageBatches.size()` when never crossed.
- `setImagePixels` returns BEFORE `m_images[id]`, so a short buffer creates no
  entry (gpu_resources_test.cpp:405 depends on this).
- `dropImage` uses `release()` + `deleteLater()`, and `imageIds()` returns a
  copy so the drop loop cannot invalidate its own iterator.
- Image UV maths: `image->empty()` is checked first, so imgW/imgH > 0; srcX+srcW
  is summed as FLOAT then clamped, so no int overflow.
- `run_splitter` scaleBreak: `flush()` sets `open=false` and resets `cur`, and
  the `if (!open)` branch sets `cur.scale` — no scale leaks across a run.
- `m_glyphSrb` is created (gpu_resources.cpp:435) before the image pipeline
  borrows it as a layout template (:545); `ensureImage` runs before
  `cb->resourceUpdate(batch)`.
- `params.rowStable` and the viewport cannot disagree in length: both come from
  `m_viewport` in the same rebuild.
