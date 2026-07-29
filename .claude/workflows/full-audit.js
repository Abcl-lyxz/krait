export const meta = {
  name: 'full-audit',
  description: 'Multi-lens audit of the current diff: correctness, hostile-input security, VT-spec conformance, perf risk, project-law compliance — findings adversarially verified before reporting',
  whenToUse: 'Before merging a milestone, after a large refactor, or when the user asks for a thorough/complete audit. For routine pre-commit checks use /review instead — it is much cheaper.',
  phases: [
    { title: 'Scope', detail: 'collect the diff and changed-file map' },
    { title: 'Audit', detail: 'five specialist lenses in parallel' },
    { title: 'Verify', detail: 'adversarial check of every finding' },
  ],
}

// ---- Phase 1: Scope -------------------------------------------------------
phase('Scope')
const scope = await agent(
  'In the Krait repo, run `git diff HEAD --stat` and `git status --porcelain`, ' +
  'plus `git diff HEAD` for the full patch. Return JSON with: files (array of ' +
  'changed paths), summary (one paragraph), and diffSample (the full diff if ' +
  'under 400 lines, else the 400 most substantive lines).',
  {
    label: 'scope-diff',
    schema: {
      type: 'object',
      required: ['files', 'summary', 'diffSample'],
      properties: {
        files: { type: 'array', items: { type: 'string' } },
        summary: { type: 'string' },
        diffSample: { type: 'string' },
      },
    },
  },
)

if (!scope || scope.files.length === 0) {
  log('Working tree is clean — nothing to audit.')
  return { verdict: 'CLEAN', findings: [] }
}
log(`Auditing ${scope.files.length} changed files`)

// ---- Phase 2: Audit (five lenses, each blind to the others) ---------------
const FINDINGS_SCHEMA = {
  type: 'object',
  required: ['findings'],
  properties: {
    findings: {
      type: 'array',
      items: {
        type: 'object',
        required: ['file', 'summary', 'severity'],
        properties: {
          file: { type: 'string' },
          line: { type: 'integer' },
          summary: { type: 'string' },
          severity: { enum: ['blocking', 'major', 'minor'] },
          failure_scenario: { type: 'string' },
        },
      },
    },
  },
}

const LENSES = [
  ['correctness', 'lifetimes, UB, iterator invalidation, use-after-move, concurrency races, missing timeouts, Qt ownership'],
  ['security', 'hostile remote input: unchecked lengths, uncapped allocations, format strings, secrets in logs/errors, OSC/DCS payload caps, paste sanitizing'],
  ['vt-spec', 'conformance drift: default params (0 vs 1), colon subparams, OSC terminators, interrupted sequences, dishonest DA/DECRQM replies, missing docs/conformance.md rows'],
  ['perf', 'hot-path regressions: allocation in byte loop, per-byte rendering, missing damage coalescing, shaping on render thread, lock contention'],
  ['project-law', 'violations of CLAUDE.md and .claude/rules/*.md for the touched paths: banned constructs (bare wcwidth, libssh2, ssh.exe, modal dialogs, plaintext secrets), missing tests-in-same-commit, missing tr() locales'],
]

phase('Audit')
const audited = await pipeline(
  LENSES,
  ([lens, detail]) => agent(
    `You are the ${lens} lens auditing a diff in the Krait terminal-emulator repo. ` +
    `Focus EXCLUSIVELY on: ${detail}. Read the governing .claude/rules/*.md for the ` +
    `touched paths first. Changed files: ${JSON.stringify(scope.files)}. ` +
    `Diff:\n${scope.diffSample}\n` +
    'Read surrounding code with Read/Grep before judging. Report only real defects ' +
    'affecting correctness/security/spec/requirements — zero style findings. ' +
    'If clean under your lens, return an empty findings array.',
    { label: `audit:${lens}`, phase: 'Audit', schema: FINDINGS_SCHEMA },
  ),
  // ---- Phase 3: Verify each lens's findings as soon as that lens finishes --
  (result, [lens]) => parallel(
    (result?.findings ?? []).map((f) => () =>
      agent(
        `Adversarially verify this ${lens} finding in the Krait repo. Try to REFUTE it: ` +
        `read the actual code at ${f.file}${f.line ? ':' + f.line : ''} and its callers. ` +
        `Finding: ${f.summary}. Claimed failure: ${f.failure_scenario ?? 'unstated'}. ` +
        'Default to refuted=true if you cannot construct a concrete failing scenario from the real code.',
        {
          label: `verify:${lens}:${(f.file || '').split('/').pop()}`,
          phase: 'Verify',
          schema: {
            type: 'object',
            required: ['refuted', 'reason'],
            properties: { refuted: { type: 'boolean' }, reason: { type: 'string' } },
          },
        },
      ).then((v) => ({ ...f, lens, verified: v ? !v.refuted : false, verifyReason: v?.reason ?? 'verifier died' })),
    ),
  ),
)

const confirmed = audited.flat().filter(Boolean).filter((f) => f.verified)
const order = { blocking: 0, major: 1, minor: 2 }
confirmed.sort((a, b) => (order[a.severity] ?? 3) - (order[b.severity] ?? 3))

log(`${confirmed.length} confirmed findings (of ${audited.flat().filter(Boolean).length} raw)`)
return {
  verdict: confirmed.some((f) => f.severity === 'blocking') ? 'BLOCKING'
    : confirmed.length ? 'FINDINGS' : 'CLEAN',
  findings: confirmed,
}
