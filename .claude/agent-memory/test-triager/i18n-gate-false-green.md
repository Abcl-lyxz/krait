---
name: i18n-gate-false-green
description: The three [i18n] tests validate the .ts files against themselves only — they never see new or changed tr() strings in C++ source, so a missing translation passes green
metadata:
  type: project
---

`tests/unit/i18n_test.cpp` (the "locale gate", T32) **cannot fail on an
untranslated new string.** All three cases parse `src/app/i18n/krait_en.ts` and
`krait_th.ts` with QDomDocument and check them against *each other*:

1. `every string has a Thai translation` — no `type="unfinished"`/`"vanished"`, no empty `<translation>`
2. `English and Thai cover the same strings` — equal message counts, same context/source pairwise
3. `a translation keeps the placeholders its source has` — `%1`..`%9` survive

Nothing runs `lupdate` or scans C++ for `tr()` / `QCoreApplication::translate()`.
A string added in source is simply **absent** from both .ts files, so all three
checks stay consistent and green. The `<location line="..."/>` attributes in the
.ts files go stale for the same reason and are a cheap tell.

**Why:** the file's own header comment claims it "fails the moment someone adds
a string and does not translate it." That is the intent, not the behavior — and
believing the comment turns the gate into a false green. Confirmed 2026-07-31 on
`t52-backend-factory`: `describeHostKey` in `src/app/error_banner.h` plus 4
`tr()` calls in `src/net/conpty/conpty_backend.cpp` added/reworded strings; the
.ts files still held the pre-T52 wording (`"...the one Krait remembers."` vs the
new `"...remembers, so the connection was stopped before anything was sent."`)
and all three [i18n] tests passed.

**How to apply:** when a task touches user-facing strings, a green [i18n] run is
**not** evidence the translations landed. Diff the source `tr()` strings against
the .ts files (or note that only a real `lupdate` run would tell), and report the
gate as inconclusive rather than passing. See [[suite-health]],
[[benign-signatures]].
