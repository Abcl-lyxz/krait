# ADR-0004: MIT license, public repository from day 1

- **Status:** accepted
- **Date:** 2026-07-29
- **Deciders:** Kla (project owner), plan-session interview

## Context

Krait's positioning against WindTerm is "actually open and actively
maintained"; the licensing choice shapes contributors, corporate users, and
that credibility. All runtime dependencies are compatible with any choice
(Qt and libssh are LGPL, dynamically linked; the rest MIT/BSD-class).

## Decision

MIT, copyright "Krait contributors". Repository public from the first commit.
No CLA. Ship third-party license texts (Qt, libssh, OpenConsole per ADR-0011,
and vcpkg deps) in the distribution.

## Alternatives considered

- GPLv3 → blocks proprietary forks but bars some corporate contributors and
  complicates the Lua-plugin story; forks are not our real risk.
- Apache-2.0 → fine, but patent-grant benefit marginal here; MIT is simpler.
- Dual MIT+commercial → requires a CLA from day 1; friction not worth it.

## Consequences

Maximum adoption path; a proprietary fork is legally possible (accepted).
LGPL compliance duties (dynamic linking, relink ability) documented in the
release checklist. Revisit trigger: a commercial edition becomes a real goal.
