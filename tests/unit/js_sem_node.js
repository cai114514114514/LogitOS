// js_sem_node -- the ORACLE half of the js_sem differential.
//
// Runs the same case files as tests/unit/js_sem.c and produces byte-identical
// framing, so the two stdouts can be diffed with no interpretation layer.
// Anything this file does that js_sem.c does not is a manufactured difference,
// which is why both output contracts are written out in both headers.
//
//   node tests/unit/js_sem_node.js <file.js>...
//
// A FRESH CONTEXT PER CASE, and that was not the first version. The first
// version ran every case in one global scope with vm.runInThisContext, and
// three cases came back as SyntaxError from NODE -- because c01 and c04 both
// declare `class Base` at top level and the second redeclaration is a
// SyntaxError in a scope that already has the first. js_sem.c builds a fresh
// JSRuntime per case, so the engine half was fine and the oracle was the
// broken one. Rule 1: suspect the apparatus first. It cost one run.
//
// A fresh vm context also means no `process`, no `console`, no `require`
// inside a case -- which is the same surface js_sem.c offers, so neither side
// can accidentally lean on a host global.

const fs = require('fs');
const vm = require('vm');
const path = require('path');

const files = process.argv.slice(2);
if (files.length === 0) { console.log('usage: js_sem_node.js <file.js>...'); process.exit(2); }

const out = [];
function emit(s) { out.push(s); }

function reportException(e) {
    let name = '(non-object throw)';
    try {
        if (e !== null && typeof e === 'object') name = e.constructor.name;
    } catch (_) { /* a throw whose constructor is unreachable */ }
    emit('! ' + name);
    try { process.stderr.write('  [exception] ' + String((e && e.stack) || e) + '\n'); } catch (_) {}
}

// node has no "drain the microtask queue now" primitive, and a setTimeout is a
// macrotask, so it lands strictly after every microtask the script queued --
// which is the checkpoint js_sem.c's JS_ExecutePendingJob loop reaches. No
// clock is read; the timeout is only an ordering barrier.
function afterMicrotasks() {
    return new Promise(res => setTimeout(res, 0));
}

(async () => {
    for (const f of files) {
        const base = path.basename(f);
        emit('## ' + base);
        let src = null;
        try { src = fs.readFileSync(f, 'utf8'); } catch (_) { src = null; }
        if (src === null) { emit('! MISSING'); emit('## end ' + base); continue; }

        const sandbox = {};
        sandbox.print = function (...args) { emit('| ' + args.map(String).join(' ')); };
        vm.createContext(sandbox);
        try {
            vm.runInContext(src, sandbox, { filename: base });
        } catch (e) {
            reportException(e);
        }
        await afterMicrotasks();
        emit('## end ' + base);
    }
    emit('JSSEM-DONE ' + files.length);
    process.stdout.write(out.join('\n') + '\n');
})();
