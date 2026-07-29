# ADR-0003: Write our own VT core (parser + grid); utf8proc for segmentation

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), research in `docs/research/landscape-2026.md §4`

## Context

The differentiation strategy (IDEAS §3.10, §3.4) depends on modern VT
extensions: kitty keyboard, kitty graphics + sixel, mode 2026/2027, styled
underlines, OSC 66 text sizing, OSC 133, honest capability replies, and
reflow-correct scrollback. The only embeddable C cores — libvterm (MIT,
powers nvim :terminal) and libtsm (dormant) — lag years behind on exactly
these, and their grid models fight logical-line reflow. Every quality
terminal of the last decade (kitty, Ghostty, Windows Terminal, WezTerm,
Alacritty, foot, Contour) wrote its own core; that unanimity is the evidence.

## Decision

- **Own VT core** in `src/core/`: UTF-8 decoder → Paul Williams DEC-style
  state machine (table-driven) → grid with **logical lines + wrap points**
  (reflow-native) → damage list. Zero dependencies on Qt/network/rendering.
- **utf8proc** (MIT) for grapheme segmentation; width = clusters + generated
  tables + VS15/16 + configurable East-Asian-Ambiguous. Bare `wcwidth()` is
  banned by rule and by review.
- Conformance is test-driven from day one: corpus derived from
  vttest/esctest + hand-written cases per sequence (tracked in
  `docs/conformance.md`), plus a libFuzzer target on the parser.
- DA/DECRQM/XTGETTCAP replies generated from the same capability table the
  implementation uses — structurally honest.

## Alternatives considered

- libvterm → stable but lags modern extensions; upstreaming our needs is
  slower than owning the core; grid model complicates reflow.
- libtsm → dormant; community forks unmaintained.
- Porting Alacritty's vte / Ghostty's core → Rust/Zig FFI seam in the hottest
  path of a C++ codebase; fights the "core is pure C++23" testability rule.

## Consequences

- We own correctness: the corpus + fuzzer are not optional overhead, they
  ARE the moat. Every sequence change lands with tests (rules/vt-core.md).
- A standalone `krait-core` build target proves the zero-dependency rule in CI.
- More upfront work in M0/M1 than embedding; ADR accepts that cost knowingly.
- Revisit trigger: none foreseeable — superseding this means the project is
  pivoting.
