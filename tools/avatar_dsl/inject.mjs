#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 Kenta IDA <fuga@fugafuga.org>
// SPDX-License-Identifier: BSL-1.0
//
// Bundle the JS DSL compiler (lexer/parser/emitter/compile/opcodes) into a
// single IIFE and inject it (+ the default DSL source text) into an HTML
// template. Used by both wasm/build.sh (standalone preview shell) and the
// firmware build (wifi_config settings page) so both stay in lockstep.
//
// Usage:
//   node tools/avatar_dsl/inject.mjs <template.html> <dsl_source.avdsl> <output.html>
//
// Placeholders in the template are replaced verbatim:
//   /*{{AVATAR_DSL_BUNDLE}}*/        → IIFE bundle (assigns window.AvatarDsl)
//   "{{AVATAR_DSL_DEFAULT_SOURCE}}"  → JSON-encoded DSL source string

import { readFileSync, writeFileSync } from 'node:fs';
import { dirname, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const args = process.argv.slice(2);
if (args.length !== 3) {
  process.stderr.write('usage: inject.mjs <template.html> <dsl_source.avdsl> <output.html>\n');
  process.exit(1);
}
const [templatePath, dslSourcePath, outPath] = args;

const SCRIPT_DIR = dirname(fileURLToPath(import.meta.url));
// Dependency-ordered concatenation. The keyword stripping is line-based and
// only handles single-line `import { ... } from '...'` / `export ` prefixes —
// matching the project's strict ESM style.
const SRC_FILES = ['opcodes.js', 'lexer.js', 'parser.js', 'emitter.js', 'compile.js'];

function buildBundle() {
  const parts = [];
  for (const fn of SRC_FILES) {
    let text = readFileSync(resolve(SCRIPT_DIR, fn), 'utf8');
    text = text.replace(/^import\s+\{[^}]*\}\s+from\s+['"][^'"]+['"];?\s*$/gm, '');
    text = text.replace(/^export\s+/gm, '');
    parts.push(`// === ${fn} ===\n${text}`);
  }
  return '(function(){\n' + parts.join('\n') +
    '\n// Public surface for the host page.\n' +
    'window.AvatarDsl = { compile: compile, MAGIC: MAGIC, VERSION: VERSION };\n' +
    '})();';
}

const bundle = buildBundle();
const dslSource = readFileSync(dslSourcePath, 'utf8');

let html = readFileSync(templatePath, 'utf8');
let bundleHits = 0, srcHits = 0;
html = html.replace(/\/\*\{\{AVATAR_DSL_BUNDLE\}\}\*\//g,
  () => { ++bundleHits; return bundle; });
html = html.replace(/"\{\{AVATAR_DSL_DEFAULT_SOURCE\}\}"/g,
  () => { ++srcHits; return JSON.stringify(dslSource); });

if (bundleHits === 0) {
  process.stderr.write(`warning: ${templatePath}: no '/*{{AVATAR_DSL_BUNDLE}}*/' placeholder found\n`);
}
if (srcHits === 0) {
  process.stderr.write(`warning: ${templatePath}: no '"{{AVATAR_DSL_DEFAULT_SOURCE}}"' placeholder found\n`);
}

writeFileSync(outPath, html);
process.stdout.write(`[avatar_dsl] injected bundle (${bundle.length}B) + DSL source (${dslSource.length}B)` +
  ` into ${templatePath} -> ${outPath}\n`);
