#!/usr/bin/env node
// PostToolUse hook (matchers: Edit|Write|NotebookEdit, and Bash).
//  - After C/C++ edits: run clang-format -i on the touched file (style is
//    the formatter's job, never Claude's) and mark sources dirty.
//  - After Bash: record successful ctest/build runs for the Stop gate.
// Node built-ins only. Must never throw; on any error, exit 0 silently.
'use strict';
const fs = require('fs');
const path = require('path');
const { spawnSync } = require('child_process');

function safe(fn, fallback) { try { return fn(); } catch { return fallback; } }

function touch(cacheDir, name) {
  safe(() => {
    fs.mkdirSync(cacheDir, { recursive: true });
    fs.writeFileSync(path.join(cacheDir, name), new Date().toISOString());
  });
}

function looksFailed(resp) {
  // Best-effort: PostToolUse generally fires on success, but Bash results can
  // carry nonzero exits. Only treat as failed when we see clear evidence.
  const s = safe(() => JSON.stringify(resp), '') || '';
  return /"(exit_?code|exitStatus)"\s*:\s*[1-9]/.test(s) || /"interrupted"\s*:\s*true/.test(s);
}

function main() {
  const input = safe(() => JSON.parse(fs.readFileSync(0, 'utf8')), {});
  const proj = process.env.CLAUDE_PROJECT_DIR || input.cwd || process.cwd();
  const cacheDir = path.join(proj, '.claude', '.cache');
  const tool = input.tool_name || '';

  if (tool === 'Bash') {
    const cmd = (input.tool_input && input.tool_input.command) || '';
    if (looksFailed(input.tool_response)) { return; }
    if (/\bctest\b/.test(cmd)) { touch(cacheDir, 'last-test-pass'); }
    if (/\bcmake\b[^|;&]*--build\b/.test(cmd) || /\bninja\b/.test(cmd)) { touch(cacheDir, 'last-build'); }
    return;
  }

  // Edit | Write | NotebookEdit
  const file = (input.tool_input && (input.tool_input.file_path || input.tool_input.notebook_path)) || '';
  if (!file) { return; }
  const rel = path.relative(proj, file).replace(/\\/g, '/');
  const isCpp = /\.(c|cc|cpp|cxx|h|hh|hpp|ixx)$/i.test(file);
  const isSource = /^(src|tests|bench)\//.test(rel) || /^CMake(Lists\.txt|Presets\.json)$/.test(rel) || /\.qml$/i.test(file);

  if (isCpp && fs.existsSync(file)) {
    // Cache formatter availability so we probe PATH only once per machine.
    const flag = path.join(cacheDir, 'clang-format-available');
    let available = safe(() => fs.readFileSync(flag, 'utf8'), null);
    if (available === null) {
      const probe = spawnSync('clang-format', ['--version'], { encoding: 'utf8', timeout: 4000 });
      available = probe.status === 0 ? 'yes' : 'no';
      touch(cacheDir, 'clang-format-available');
      safe(() => fs.writeFileSync(flag, available));
    }
    if (available === 'yes') {
      spawnSync('clang-format', ['-i', file], { cwd: proj, timeout: 10000 });
    }
  }

  if (isSource) { touch(cacheDir, 'src-dirty'); }
}

safe(main);
process.exit(0);
