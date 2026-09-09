// MINIMAL: does an early exit from `for await...of` AWAIT the iterator's
// close before running the code after the loop?
//
// Spec: an abrupt completion in a for-await-of body performs AsyncIteratorClose,
// which calls return() and AWAITS the result before the completion continues.
// So the generator's finally must print BEFORE the statement after the loop.
// This is where an async resource is released -- a stream reader, a lock, a
// subscription -- and every one of those is written as `try { ... } finally
// { release() }` inside an async iterator.
'use strict';

async function* gen() {
  try { yield 'a'; yield 'b'; }
  finally { print('1 finally'); }
}

(async () => {
  for await (const v of gen()) { print('1 body ' + v); break; }
  print('1 after');
})();

// the same shape with `return` instead of `break`
async function* gen2() {
  try { yield 'a'; }
  finally { print('2 finally'); }
}
(async () => {
  async function f() { for await (const v of gen2()) { print('2 body ' + v); return 'R'; } }
  print('2 after ' + await f());
})();

// the same shape with a THROW out of the body
async function* gen3() {
  try { yield 'a'; }
  finally { print('3 finally'); }
}
(async () => {
  try { for await (const v of gen3()) { print('3 body ' + v); throw new Error('x'); } }
  catch (e) { print('3 caught ' + e.message); }
  print('3 after');
})();

// an ASYNC finally: the close must await the awaits inside the finally too
async function* gen4() {
  try { yield 'a'; }
  finally { await null; print('4 finally after await'); }
}
(async () => {
  for await (const v of gen4()) { print('4 body ' + v); break; }
  print('4 after');
})();

// the SYNC control: the same shape with a plain generator and for-of, where
// the close is synchronous and cannot be deferred. If this one also comes out
// reordered the finding is about generators; if only the async ones move, it
// is about the await in AsyncIteratorClose.
function* sgen() { try { yield 'a'; yield 'b'; } finally { print('5 finally'); } }
for (const v of sgen()) { print('5 body ' + v); break; }
print('5 after');

// explicit .return() on an async generator, awaited by hand
async function* gen6() { try { yield 'a'; } finally { print('6 finally'); } }
(async () => {
  const it = gen6();
  print('6 next ' + (await it.next()).value);
  print('6 return ' + JSON.stringify(await it.return('Z')));
  print('6 after');
})();

// for-await over a SYNC iterable with break: closing calls the sync return()
const syncIt = {
  [Symbol.iterator]() {
    let i = 0;
    return { next: () => ({ value: 's' + (++i), done: i > 3 }),
             return(v) { print('7 sync return'); return { value: v, done: true }; } };
  }
};
(async () => {
  for await (const v of syncIt) { print('7 body ' + v); break; }
  print('7 after');
})();
