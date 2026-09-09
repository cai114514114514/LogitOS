// Labelled break/continue out of nested loops, and try/finally interacting
// with return/break/continue. Minifiers rewrite control flow into exactly
// these shapes, so a wrong answer here is a wrong answer in shipped code that
// was correct in source.
'use strict';

outer:
for (let i = 0; i < 3; i++) {
  inner:
  for (let j = 0; j < 3; j++) {
    if (j === 1) continue outer;
    if (i === 2) break outer;
    print('ij ' + i + j);
  }
}

// a label on a block, broken out of
blk: {
  print('in block');
  if (true) break blk;
  print('NOT REACHED');
}
print('after block');

// labelled continue from inside a switch inside a loop
loop:
for (let i = 0; i < 4; i++) {
  switch (i) {
    case 1: continue loop;
    case 3: break loop;
    default: print('sw ' + i);
  }
  print('tail ' + i);
}

// finally overrides a return value
function f1() { try { return 'try'; } finally { print('f1 finally'); } }
print('f1 ' + f1());
function f2() { try { return 'try'; } finally { return 'finally'; } }
print('f2 ' + f2());
function f3() { try { throw new Error('e'); } finally { return 'swallowed'; } }
print('f3 ' + f3());

// finally with break inside a loop swallows the exception
function f4() {
  for (let i = 0; i < 2; i++) {
    try { throw new Error('x'); } finally { break; }
  }
  return 'f4 done';
}
print('f4 ' + f4());

// a return in try, then a continue in finally
function f5() {
  const out = [];
  for (let i = 0; i < 3; i++) {
    try { if (i === 1) continue; out.push('t' + i); } finally { out.push('f' + i); }
  }
  return out.join(',');
}
print('f5 ' + f5());

// nested finally ordering, including one that throws
function f6() {
  try {
    try { throw new Error('inner'); }
    finally { print('f6 inner finally'); }
  } catch (e) { print('f6 caught ' + e.message); }
  finally { print('f6 outer finally'); }
  return 'f6 done';
}
print(f6());

function f7() {
  try { try { return 'A'; } finally { throw new Error('B'); } }
  catch (e) { return 'caught ' + e.message; }
}
print('f7 ' + f7());

// optional catch binding
try { throw 1; } catch { print('no binding'); }

// let/const per-iteration binding captured by closures
const fns = [];
for (let i = 0; i < 3; i++) fns.push(() => i);
print('let capture ' + fns.map(f => f()).join(','));
const vfns = [];
for (var v = 0; v < 3; v++) vfns.push(() => v);
print('var capture ' + vfns.map(f => f()).join(','));

// per-iteration binding in for-of and the closure inside a try/finally
const ofs = [];
for (const c of 'abc') { try { ofs.push(() => c); } finally { /* nothing */ } }
print('of capture ' + ofs.map(f => f()).join(','));

// do-while with a labelled continue
let n = 0;
dw: do { n++; if (n < 3) continue dw; } while (n < 5);
print('dw ' + n);
