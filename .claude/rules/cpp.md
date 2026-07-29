---
paths:
  - "src/**/*.{c,cc,cpp,cxx,h,hh,hpp,ixx}"
  - "tests/**/*.{c,cc,cpp,cxx,h,hh,hpp}"
  - "bench/**/*.{cpp,h,hpp}"
---

# C++ rules

- C++23 (`/std:c++23preview` until VS2026 finishes `/std:c++23`). No modules
  (tooling not ready); header/impl split, `#pragma once`.
- Ownership: `std::unique_ptr` by default; no owning raw pointers; raw
  pointers/references = non-owning borrows only. Every Qt `new` with a parent
  gets a `// owned by <parent>` comment; parentless `new` is a defect.
- No exceptions across module boundaries. `src/core/` and `src/net/` return
  `std::expected<T, Error>`; the app layer may translate to UI errors.
- Concurrency: every wait has a timeout; every thread is named at spawn;
  cross-thread Qt signal connections are explicitly `Qt::QueuedConnection`.
  Data shared across threads is either immutable, message-passed, or wrapped
  in a clearly named mutex — never "synchronized by comment".
- Warnings-as-errors (`/W4` + curated). clang-tidy config is law; do not
  suppress with NOLINT without a comment explaining why.
- Formatting belongs to clang-format (the post-edit hook runs it). Never
  spend tokens hand-aligning code.
- Every behavior change lands with a unit test in the same commit. Prefer
  running the single affected test binary, not the whole suite, while
  iterating; full suite via `test-triager` before commit.
- Includes: include-what-you-use discipline; forward-declare in headers where
  possible. Grouping/order is enforced by .clang-format — don't fight it.
- Naming (Qt-adjacent): `PascalCase` types, `camelCase` functions/methods,
  `m_camelCase` members, `snake_case` file names, `kPascalCase` constants.
- Avoid over-engineering: no error handling for states that cannot happen,
  no abstractions with a single implementation (except the `IBackend` seam,
  which exists by design), the minimum complexity for the current task.
