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

**Why:** these were deliberately accepted as fine-for-now in a skeleton commit; they become defects only when later milestones touch them.
**How to apply:** when a review touches src/core deps, root flags, or T11 targets, check these first.
