#!/usr/bin/env node
// Inline obfuscator.io string-table lookups in the recovered TCOAAL script.
// Only the self-contained prelude (string array + rotation + accessor) is
// evaluated, inside a vm sandbox; the rest of the game script is never run.
'use strict';
const fs = require('fs');
const vm = require('vm');

const src = fs.readFileSync(process.argv[2], 'utf8');
const m = src.match(/^const (_0x\w+)=\[[\s\S]*?\];\(function[\s\S]*?\}\(\1,0x[0-9a-f]+\)\);const (_0x\w+)=function[\s\S]*?return _0x\w+;\};/);
if (!m) throw new Error('string-table prelude not found');
const [prelude, , accessor] = m;
const lookup = vm.runInNewContext(prelude + accessor, {});

// Collect every alias of the accessor (const _0xabc=_0x1f07; inner re-aliases).
const aliases = new Set([accessor]);
for (let grew = true; grew;) {
  grew = false;
  for (const [, a, b] of src.matchAll(/(_0x\w+)=(_0x\w+)[,;]/g))
    if (aliases.has(b) && !aliases.has(a)) { aliases.add(a); grew = true; }
}

let out = src.slice(prelude.length).replace(/(_0x\w+)\((0x[0-9a-f]+)\)/g, (all, fn, idx) =>
  aliases.has(fn) ? JSON.stringify(lookup(parseInt(idx, 16))) : all);
// obj["prop"] -> obj.prop where safe
out = out.replace(/\[("[A-Za-z_$][\w$]*")\]/g, (_, s) => '.' + JSON.parse(s));
process.stdout.write(out);
