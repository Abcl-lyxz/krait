---
name: review
description: Review the current diff with the project's review battery before committing. Use proactively before every commit and whenever asked to review changes, a diff, or recent work.
allowed-tools: "Bash(git diff:*), Bash(git status:*), Bash(git log:*), Read, Grep, Glob, Agent"
---

Review the working tree diff (staged + unstaged) with the right battery.
Style is the formatter's job — reviews here are about correctness, security,
spec conformance, and project law.

1. Scope the diff: `git status --porcelain` and `git diff HEAD --stat`.

2. Pick the battery:
   - Trivial diff (≤ ~20 lines, docs/comments/rename only): review inline
     yourself against the governing `.claude/rules/*.md`; no subagents.
   - Anything else: launch the `cpp-reviewer` subagent on the diff.
   - `src/core/` parsing/width/grid touched: ALSO launch `vt-spec-auditor`
     (parallel with cpp-reviewer, single message).
   - Parser byte loop, grid, damage, or renderer hot path touched: ALSO
     launch `perf-auditor`.

3. Merge results: deduplicate, rank by severity, drop anything that is pure
   style or taste. Present at most 10 findings — if there are more, the top
   10 and a count.

4. Verdict line, always last:
   - `REVIEW CLEAN — safe to commit` (zero findings), or
   - `REVIEW: n findings, k BLOCKING` — blocking findings must be fixed or
     explicitly waived by the user before committing.

Do not fix findings silently in the same breath as reviewing — report first,
then fix on request (or when the fix is a one-liner, propose it inline with
the finding).
