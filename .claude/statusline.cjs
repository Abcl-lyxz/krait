#!/usr/bin/env node
// Status line: Krait │ model │ branch(+dirty) │ phase │ context %
// Reads the status JSON from stdin when provided; degrades gracefully when not.
// Node built-ins only, fast, never throws.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }

const C = {
  dim: '\x1b[2m', reset: '\x1b[0m', bold: '\x1b[1m',
  green: '\x1b[32m', yellow: '\x1b[33m', cyan: '\x1b[36m', magenta: '\x1b[35m', red: '\x1b[31m',
};

const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {}) || {};
const proj = (input.workspace && (input.workspace.project_dir || input.workspace.current_dir)) ||
  process.env.CLAUDE_PROJECT_DIR || process.cwd();

const model = (input.model && (input.model.display_name || input.model.id)) || '';

const branch = safe(() => {
  const r = spawnSync('git', ['rev-parse', '--abbrev-ref', 'HEAD'], { cwd: proj, encoding: 'utf8', timeout: 2000 });
  if (r.status !== 0) { return ''; }
  const b = r.stdout.trim();
  const s = spawnSync('git', ['status', '--porcelain'], { cwd: proj, encoding: 'utf8', timeout: 2000 });
  const n = s.status === 0 ? s.stdout.split('\n').filter(Boolean).length : 0;
  return n > 0 ? `${b}${C.yellow}*${n}${C.reset}` : b;
}, '');

const phase = safe(() => {
  const m = fs.readFileSync(path.join(proj, 'STATE.md'), 'utf8').match(/^Phase:\s*(.+)$/m);
  return m ? m[1].split('—')[0].trim() : '';
}, '');

// Context usage: field names vary across versions — probe the known shapes.
const ctx = safe(() => {
  const u = input.context_window_usage || input.context_window || input.context || {};
  const pct = u.used_percentage ?? u.used_percent ?? u.percent ??
    (u.used_tokens && u.max_tokens ? Math.round((u.used_tokens / u.max_tokens) * 100) : null);
  if (pct == null) { return ''; }
  const color = pct >= 80 ? C.red : pct >= 60 ? C.yellow : C.green;
  return `${color}ctx ${Math.round(pct)}%${C.reset}`;
}, '');

const parts = [
  `${C.bold}${C.magenta}Krait${C.reset}`,
  model && `${C.cyan}${model}${C.reset}`,
  branch && `${C.green}${branch}${C.reset}`,
  phase && `${C.bold}${phase}${C.reset}`,
  ctx,
].filter(Boolean);

process.stdout.write(parts.join(`${C.dim} │ ${C.reset}`));
process.exit(0);
