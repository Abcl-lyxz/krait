---
name: t17-audit-findings
description: T17 SGR-extended audit — legacy unknown-kind and mixed-separator consumption bugs; the "count subparams" Pi rule is correct
metadata:
  type: project
---

# T17 (SGR extended) audit — 2026-07-29

Audited `src/core/parser/sgr.cpp` `readExtendedColor()` against xterm
`parse_extended_colors()` and kitty `cursor_from_sgr()`/`_parse_sgr()`.
Spec facts live in [[spec-sources]].

**Verdicts that were right and should not be re-litigated:**

- Pi-by-subparameter-count (`subCount >= 5 ? 3 : 2`) is exactly what both
  references do, tolerance parameters included.
- Rejecting (not clamping) out-of-range channels while still consuming them is
  xterm's behaviour, byte for byte.
- SGR 21 = double underline is confirmed by both references.
- `ul.kind == Default` meaning "follow fg" is the correct model of kitty's
  reverse-video rule.

**Deviations found (all in argument *consumption*, all one-liners):**

1. Legacy form + unknown kind (`38;7;1;2`) consumes only the introducer, so the
   kind is re-read as its own SGR and `7` turns on reverse video. Both
   references consume the kind. Fix: `consumed = 2` in that fallthrough.
2. The legacy branch ignores subparam flags entirely, so `38;2:0:1:2:3` misreads
   Pi as red and leaks the leftover as an SGR, and `38;5;1:7` promotes a
   trailing subparam to a base SGR. Root cause: `next = i + ext.consumed` can
   land on an index whose `subparam[]` flag is true, breaking the invariant the
   top of the loop maintains.
3. `4:n` for n > 5 falls back to Single; kitty clamps to 5 (dashed).

**Why:** the class of bug here is always "how many parameters did that swallow",
never "what colour did it compute" — the colour maths was clean on first read.

**How to apply:** when auditing any future subparameter-bearing sequence, trace
the index the consumer advances to and assert it never lands on a `subparam[]`
index. That single check would have caught 2 of the 3 findings above.
