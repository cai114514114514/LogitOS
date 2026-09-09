// node_run.js -- the ORACLE side of the tests/jssem differential.
//
// Runs one case file under node with exactly the surface the QuickJS runner
// gives it: a global `print` and nothing else.  vm.runInThisContext is used
// rather than require() so the case is a GLOBAL SCRIPT on both sides -- a
// CommonJS module has a function scope, `var` does not reach globalThis, and
// `this` is not the global object, all three of which change observable
// behaviour in cases that touch scope.
//
// print() must stringify the way QuickJS's JS_ToCString does, NOT the way
// console.log does: console.log pretty-prints objects ("[Object: null
// prototype] {}"), which would make every case containing an object diff for a
// formatting reason instead of a semantic one.  String(x) is the shared
// definition.
'use strict';
const fs = require('fs');
const vm = require('vm');
const path = process.argv[2];
if (!path) { process.stderr.write('usage: node node_run.js <file.js>\n'); process.exit(2); }
const src = fs.readFileSync(path, 'utf8');

globalThis.print = function () {
  const a = [];
  for (let i = 0; i < arguments.length; i++) a.push(String(arguments[i]));
  process.stdout.write(a.join(' ') + '\n');
};

try {
  vm.runInThisContext(src, { filename: path, displayErrors: false });
} catch (e) {
  // Match the QuickJS runner's shape byte for byte.  QuickJS's JS_ToCString of
  // an Error gives "TypeError: msg"; node's String(e) gives the same.
  process.stdout.write('EXCEPTION[eval]: ' + String(e) + '\n');
}
