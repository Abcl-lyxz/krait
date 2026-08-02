# STATE

Phase: M5 — beauty + protocol completeness. Feature work done on
`t75-m5-beauty`, with one large gap named below. In review.

## Now

M4 merged (`3fba78c`). M5 is nine commits on `t75-m5-beauty`, branched from it.
**589 tests green**, up from 529. Build clean under `/W4 /WX`, clang-format
clean on all 41 changed C++ files, clang-tidy clean on the gated
`bugprone-*`/`concurrency-*` checks, and `tests/core-standalone` still links
with no Qt on the path — which is what proves the two new image decoders did
not smuggle a dependency into `src/core/`.

| Task | What landed |
|---|---|
| T75 | `src/app/theme/` — the theme system. Model, 4 builtins, TOML round trip |
| T76 | Importers: iTerm2 `.itermcolors`, Windows Terminal schemes, base16 YAML |
| T77 | `qml/ThemeGallery.qml` — gallery + live editor; 101 QML hex literals gone |
| T78 | Background image: `[background]` image/opacity/fit, alpha in the clear |
| T79 | `src/core/graphics/sixel.*` — sixel decoder + `grid/images.*` placements |
| T80 | `src/core/graphics/kitty.*` — kitty graphics over a new APC parser state |
| T81 | OSC 66 text sizing, scale packed into `Attr::flags`' spare bits |
| T82 | Mode 2048, in-band resize notification |
| T83 | Mode 2031 + OSC 4/10/11/12 and 104/110/111/112 |

## Next task (exactly one)

**The renderer draws no images and no scaled text.** Everything upstream of the
GPU is done and tested: sixel and kitty both decode, placements are anchored to
stable line indices and survive scroll/eviction/reflow, OSC 66 reserves the
right cells and advances the cursor correctly. Nothing paints any of it. So
today `kitty icat` and `img2sixel` transmit successfully, get an `OK` back, and
show nothing; OSC 66 text lays out at the right size and draws at 1x.

That is M5's one real gap and it is the whole of the next task. What it needs:

1. A `QRhiTexture` per image, cached by image id, uploaded on first use and
   dropped with the image. `GpuResources` already owns the atlas texture and
   the device-lost rebuild path — this rides the same lifecycle.
2. An `ImageInstance` array and a third pipeline. The glyph pipeline is close
   but samples an alpha-only atlas; images need an RGBA sampler, so it is a new
   shader pair in `src/render/shaders/` rather than a reuse.
3. `FrameBuilder` emits the quads, resolving each `Placement::anchor` through
   `Scrollback::indexOfStable()` to a viewport row and skipping placements that
   scrolled out. Negative `zIndex` draws BEFORE the text, everything else after.
4. Damage: a placement's rows must be dirtied when the viewport moves, or an
   image will smear on scroll.
5. Then the milestone's demo gate is runnable: `notcurses-demo`, `img2sixel`,
   `kitty icat`.

`docs/conformance.md` states this gap in both the Graphics and OSC rows rather
than implying it by silence.

## Open questions

- **Nothing in M5 has been run by a human either.** Same as M4. The theme
  gallery, the live editor, the file picker and the background image are all
  verified by reading and by the model tests underneath them. **user-decides**
  whether to do a manual pass before merge.
- **Acrylic/blur is CUT, not forgotten.** `docs/plan/01-milestones.md` puts it
  on M5's cut line ("CRT shader → acrylic → iTerm2 image protocol"). It is a DWM
  backdrop attribute needing a composited desktop, and nothing in an unattended
  session can verify it. **The CRT shader is cut for the same reason** and is
  first on that list.
- **M3's serial demo still has never been run** — carried over, unchanged.
- **First-run discoverability** — carried over from M3 and M4, now slightly
  worse: M5 adds two palette commands and no new shortcut. **user-decides**.

## Watchouts

- **`main` is protected.** Branch + PR, always.
- **The build shell has no dev environment.** vcvars64 (VS **18** Community)
  plus `QT_ROOT=C:\Qt\6.10.3\msvc2022_64`; `VCPKG_ROOT` comes from vcvars.
  A hung ctest holds `krait-qt-tests.exe` open and the next link fails LNK1168:
  `taskkill /F /IM krait-qt-tests.exe` first.
- **A failed build leaves the OLD test exe in place, and ctest then reports
  "All tests passed" from it.** Never read a ctest result without confirming
  the build before it exited 0.
- **Three `krait-app.exe` exist.** `build/dev/` is current.
- **`Read` returns only line 1** of any file the claude-mem hook has
  observations on. Use `Grep` with pattern `^`, `output_mode: content`,
  `head_limit: 0` — or `sed -n 'A,Bp'` for a range.
- **Writing C++ through `node -e` from Bash eats `$` and collapses `\x1b` into
  a raw ESC byte.** It happened three times this session. MSVC accepts the raw
  byte and a human reading the file cannot see it. Use a `.mjs` file in the
  scratchpad, or the Edit tool.
- **`/wd4702` is on krait-app only** (`src/app/CMakeLists.txt`), for a defect in
  Qt's own `qjsengine.h`. It fires the moment qmlcachegen sees a .qml file call
  a C++ singleton method returning QString or bool — which any future QML will.
  Everything else keeps `/W4 /WX` whole.

## Not covered by any automated test — M5's honest list

- **Every QML surface added this milestone**: the gallery, the live editor, the
  file picker, the background image layer. There are still no QML tests in this
  repo. The C++ underneath them (`ThemeStore`, `ThemeModel`'s token map, the
  importers) is tested; the bindings are not.
- **The renderer half of graphics and OSC 66** — because it does not exist. See
  "Next task".
- **`ThemeStore`'s file IO**: `save()`, `importFile()` and the directory scan
  are exercised only through `ThemeModel` by reading. The pure halves
  (`parseToml`, `toToml`, the three importers, `themeFileName`) have 12 cases.
- **No sixel or kitty FUZZ RUN has been done**, only seeds. 1385 seeds now
  exist and `tests/fuzz/run-smoke.cmd` needs the clang-cl fuzz preset, which
  this session did not build. The two decoders allocate from remote-declared
  sizes, so this is the highest-value unrun gate in the tree.
- **`interleaveShell()` is still entirely uncovered** — carried from M4,
  unchanged, still the biggest structural test gap.

## Known defects, deliberately not fixed in M5

- **Sixel and kitty images are invisible.** See "Next task". A gap rather than a
  defect, but it presents to a user as one.
- **A sixel or kitty image is not erased by anything.** ED/EL clear cells and
  OSC 133 marks; they do not drop placements covering those cells. A `clear`
  will leave a picture on screen once the renderer draws them.
- **Kitty's `a=d` has no selectors.** A bare `a=d` drops all placements and
  keeps the images (kitty's `d=a` default); `d=i`, `d=z`, `d=p` and the
  uppercase "also free the image" variants are not read.
- **Unicode placeholders (`U=1`) are not implemented**, so an image placed
  through them will not appear even once the renderer lands.
- **Carried from M4, all unchanged**: the `D`-on-an-older-prompt case when a
  marked line is destroyed under a scroll region; quake mode having no palette
  entry; the mouse-only shell-integration confirmation; telnet/raw/serial
  declaring `reconnecting` with nothing listening; `~TerminalItem` joining the
  backend on the GUI thread; trigger highlights re-deriving per chunk.

## Facts verified during M5 — do not re-derive

- **Mode 2048**: enabling it MUST report the current size immediately. Reply is
  `CSI 48 ; rows ; cols ; height_px ; width_px t` — **height before width in
  BOTH pairs**, and the pixel figures are the TEXT AREA, excluding padding.
- **Mode 2031**: notification is `CSI ? 997 ; 1 n` for dark, `; 2 n` for light.
  Setting the mode sends nothing.
- **Sixel colour components are PERCENTAGES, 0-100**, not bytes. A sixel byte's
  **least significant bit is the TOP pixel**. DEC's HLS hue origin is 120° off
  the usual one — 0 is BLUE.
- **Sixel's "zero bits are set to the background" cannot be taken literally** —
  it would make multi-colour images impossible, because overprinting with `$`
  is how they are built. libsixel and xterm both treat P2 as deciding what an
  UNWRITTEN pixel is instead.
- **Kitty graphics**: `f=24` has no alpha, so it decodes OPAQUE. Chunks are
  ≤4096 base64 bytes and a multiple of 4. Replies go only to senders that
  supplied `i=` or `I=`; `q=1` suppresses OK, `q=2` also suppresses errors.
  `t=f`/`t=t`/`t=s` name a path on the TERMINAL's machine — a file-disclosure
  primitive over SSH, refused here on security grounds.
- **OSC 66's metadata is COLON separated**, while `;` separates it from the
  text. Payload cap 4096 bytes; `s` 1-7, `w` 0-7, `n`/`d` 0-15 with `d > n`.
- **`Attr::flags` bit 3 and bits 8-15 were free**, which is the only reason
  OSC 66's scale fits without growing `Cell` past its pinned 20 bytes.
- **Qt's `qjsengine.h` has an `if constexpr` return chain with an unreachable
  fallback return**, so C4702 fires through any qmlcachegen TU that calls a
  singleton method returning QString or bool.
- M4's and M3's libssh facts still hold — see `git show 385dbaa:STATE.md`.
