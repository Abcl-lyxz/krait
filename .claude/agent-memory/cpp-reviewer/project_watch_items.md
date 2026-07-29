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

**Why:** these were deliberately accepted as fine-for-now in a skeleton commit; they become defects only when later milestones touch them.
**How to apply:** when a review touches src/core deps, root flags, or T11 targets, check these first.
