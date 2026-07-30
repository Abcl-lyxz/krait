# T19 groundwork: what utf8proc 2.11.3 already gives us

Measured on 2026-07-30 against the version vcpkg resolves from our pinned
baseline, by compiling and running a probe against the installed headers — not
from documentation and not from memory. Re-run the probe before trusting this
if the baseline moves.

```
utf8proc 2.11.3, Unicode/UCD 17.0.0
```

## The headline: `tools/gen-width-tables.py` is not needed

`docs/plan/02-m0-tasks.md` T19 calls for "generated width tables
(`tools/gen-width-tables.py` from UCD 17)". **utf8proc 2.11.3 already ships
UCD 17.0.0 tables**, which is the exact version that row asks for. Generating
our own would be a second source of truth that can only drift from the
segmentation tables we are already required by ADR-0003 to use.

Deviation taken: no generator. T19 uses utf8proc for both segmentation and
per-codepoint width, and owns only the CLUSTER-level rules on top. If a future
Unicode revision lands in the UCD before utf8proc ships it, that is the moment
to revisit — and the fix is bumping the vcpkg baseline, not writing a
generator.

## Verified API surface

`find_package(utf8proc CONFIG REQUIRED)` → target `utf8proc::utf8proc`
(SHARED IMPORTED). `find_package(unofficial-utf8proc)` also resolves but
prints a deprecation warning — do not use it.

| Function | Contract, verbatim from the installed header |
|---|---|
| `utf8proc_grapheme_break_stateful(cp1, cp2, int32_t* state)` | UAX#29 extended grapheme clusters. State starts at 0 and "must be called IN ORDER on ALL potential breaks in a string"; safe to reset to 0 after a break. Passing null skips GB10/12/13 — do not pass null. |
| `utf8proc_charwidth(cp)` | wcwidth-like, but returns **0** for non-printable rather than -1. |
| `utf8proc_charwidth_ambiguous(cp)` | True for East Asian width class A. |
| `utf8proc_category(cp)` / `_category_string(cp)` | General category. |
| `utf8proc_unicode_version()` | "17.0.0" here. |

## Measured behavior, and the traps in it

| Codepoint | width | ambiguous | cat | Note |
|---|---|---|---|---|
| `A` U+0041 | 1 | no | Lu | |
| 漢 U+6F22 | 2 | no | Lo | |
| 가 U+AC00 | 2 | no | Lo | Hangul syllable already wide |
| 🧑 U+1F9D1 | 2 | no | So | emoji base is wide without VS16 |
| 🌾 U+1F33E | 2 | no | So | |
| ZWJ U+200D | 0 | no | Cf | |
| VS15 U+FE0E | 0 | **yes** | Mn | |
| VS16 U+FE0F | 0 | **yes** | Mn | |
| ⛄ U+26C4 | 2 | no | So | |
| ⚠ U+26A0 | 1 | no | So | text-presentation default; VS16 must promote it |
| U+0301 combining acute | 0 | **yes** | Mn | |
| ก U+0E01 | 1 | no | Lo | |
| ่ U+0E48 Thai mai-ek | 0 | no | Mn | |
| ะ U+0E30 Thai sara-a | 1 | no | Lo | SPACING vowel — starts a new cluster |
| 🇦 U+1F1E6 regional indicator | 1 | no | So | a flag PAIR is two of these |
| soft hyphen U+00AD | 1 | **yes** | Cf | |
| ─ U+2500 | 1 | yes | So | genuine EAA |
| α U+03B1 | 1 | yes | Ll | genuine EAA |

**Trap 1 — `charwidth_ambiguous` is not "apply the EAA setting here".** It is
true for combining marks (U+0301, VS15, VS16) and for soft hyphen, none of
which should ever become 2 cells. The EAA setting may only promote a codepoint
that is *already width 1 and printable*. Gate on width first, ambiguity
second, and never on Mn/Cf.

**Trap 2 — VS16 has width 0, so promotion cannot be additive.** ⚠ U+26A0 is
width 1; ⚠️ (U+26A0 U+FE0F) must be 2. That is a cluster rule about the base,
not a sum over codepoints. Symmetrically VS15 must demote an emoji-presentation
base to 1.

## Segmentation results that pin the gate tests

```
🧑 <ZWJ> 🌾   breaks: [join] [join]   -> ONE cluster
ก + ่ + ะ      breaks: [join] [BREAK]  -> TWO clusters
```

- The farmer emoji is one cluster whose base is width 2, so **cluster width 2**
  — the T19 acceptance case, and it falls out of "width of the base" with no
  special ZWJ handling.
- Thai `ก่ะ` is **two** clusters, `ก่` (width 1, the mai-ek is a width-0 Mn) and
  `ะ` (width 1). Sara-a is a spacing vowel in category Lo, so UAX#29 correctly
  breaks before it. Any test asserting "Thai renders as one cluster" is wrong;
  the correct assertion is two clusters of width 1 each.
- Regional indicators are width 1 each, and GB12/13 pairing depends on the
  stateful call. A flag is one cluster containing two width-1 codepoints, so
  "width of the base" alone yields 1 — flags need an explicit rule or they
  render half-width. This is the one case where the base-width shortcut fails.

## What T19 therefore has to build

1. `src/core/unicode/width.{h,cpp}`: cluster iteration over a codepoint span
   using the stateful break function, plus a cluster-width function
   implementing base width → VS15/VS16 override → EAA setting → regional
   indicator pairing.
2. Link `utf8proc::utf8proc` into `krait-core`, and teach
   `tests/core-standalone` to find it. Note the zero-dep proof's real job is
   proving no Qt/network/render in `src/core/` — `.claude/rules/vt-core.md`
   already names utf8proc and the STL as the permitted externals, so this is
   the proof being made accurate, not weakened.
3. Mode 2027 as the negotiation signal only; the internal model is always
   cluster-based regardless (CLAUDE.md landmine).
