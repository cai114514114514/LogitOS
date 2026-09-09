// Generators: the delegation, return and throw paths, which is what every
// saga/effect runtime (redux-saga, mobx flows, co) is built out of.
'use strict';

function* g1() { yield 1; yield 2; return 3; }
print('spread ' + [...g1()].join(','));
const it = g1();
print('next ' + JSON.stringify(it.next()));
print('next ' + JSON.stringify(it.next()));
print('next ' + JSON.stringify(it.next()));
print('next ' + JSON.stringify(it.next()));

// two-way: the value passed to next() is the value of the yield expression
function* echo() { const a = yield 'first'; print('got ' + a); const b = yield 'second'; print('got ' + b); return 'done'; }
const e = echo();
print('e ' + e.next().value);
print('e ' + e.next('A').value);
print('e ' + JSON.stringify(e.next('B')));

// yield* delegates next/return/throw AND takes the inner return value
function* inner() { yield 'i1'; yield 'i2'; return 'inner-ret'; }
function* outer() { const r = yield* inner(); print('delegated returned ' + r); yield 'o1'; }
print('outer ' + [...outer()].join(','));

// return() runs finally blocks
function* fin() {
  try { yield 'a'; yield 'b'; }
  finally { print('finally ran'); }
}
const fi = fin();
print('fi ' + fi.next().value);
print('fi ' + JSON.stringify(fi.return('early')));
print('fi ' + JSON.stringify(fi.next()));

// for-of with break calls return() on the iterator
function* fin2() { try { yield 1; yield 2; yield 3; } finally { print('for-of finally'); } }
for (const v of fin2()) { print('loop ' + v); if (v === 2) break; }

// throw() into a generator is caught by the generator's own try
function* catcher() {
  try { yield 'x'; } catch (err) { print('caught ' + err); yield 'recovered'; }
  yield 'after';
}
const c = catcher();
print('c ' + c.next().value);
print('c ' + c.throw('boom').value);
print('c ' + c.next().value);

// a generator object is its own iterator and its proto chain is shared
const ga = g1(), gb = g1();
print('same proto ' + (Object.getPrototypeOf(ga) === Object.getPrototypeOf(gb)));
print('self iterable ' + (ga[Symbol.iterator]() === ga));
print('toStringTag ' + Object.prototype.toString.call(ga));

// yielding inside a loop with a labelled break
function* lab() {
  outerLoop:
  for (let i = 0; i < 3; i++) {
    for (let j = 0; j < 3; j++) {
      if (i === 1 && j === 1) break outerLoop;
      yield i + ':' + j;
    }
  }
}
print('lab ' + [...lab()].join(' '));
