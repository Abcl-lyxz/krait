---
name: handoff
description: Rewrite STATE.md at the end of a session so the next session starts sharp. Manual-only ritual — run it as the last action of every working session.
disable-model-invocation: true
allowed-tools: "Read, Write, Edit, Bash(git status:*), Bash(git log:*), Bash(git diff:*)"
---

End-of-session ritual. A session that ends without this strands the next one.

1. Gather ground truth: `git status --porcelain`, `git log --oneline -5`,
   and your own knowledge of what happened this session. If uncommitted work
   exists, say so prominently — the next session must know.

2. **Rewrite STATE.md completely** (never append) using exactly this
   skeleton:

   ```markdown
   # STATE

   Phase: <M-number or P0> — <one-line description>

   ## Now
   <2-4 sentences: what is true right now — what works, what is mid-flight>

   ## Next task (exactly one)
   <The single next task, concrete enough to start cold. Include the file
   paths and the verification command that proves it done.>

   ## After that
   <1-3 bullets of the queue, one line each>

   ## Open questions
   <decisions pending, with owner: user-decides vs plan-decides>

   ## Watchouts
   <landmines discovered this session — the things that bit you>
   ```

3. Keep it under 60 lines. The `Phase:` line must stay in exactly that
   format — the status line parses it.

4. If this session made a decision that contradicts or extends an ADR, write
   the new ADR now (docs/decisions/, next number, template in
   ADR-TEMPLATE.md) — do not bury decisions in STATE.md.

5. Confirm to the user: phase, the one next task, and whether the tree is
   clean or dirty. If dirty, recommend the commit but do not commit without
   being asked.
