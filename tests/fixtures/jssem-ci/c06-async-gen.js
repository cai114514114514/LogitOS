// Async generators and for-await-of. The ORDER of the printed tags is the
// answer; nothing here uses a clock.
'use strict';

async function* ag() {
  print('ag start');
  yield 1;
  print('ag resumed');
  yield await Promise.resolve(2);
  print('ag before return');
  return 3;
}

(async () => {
  print('A: before loop');
  for await (const v of ag()) print('A: got ' + v);
  print('A: after loop');
})();

print('sync after A');

// for-await-of over a SYNC iterable of promises awaits each value
(async () => {
  const arr = [Promise.resolve('p1'), 'plain', Promise.resolve('p3')];
  for await (const v of arr) print('B: ' + v);
  print('B: done');
})();

// for-await-of falls back to Symbol.iterator when Symbol.asyncIterator absent
const syncOnly = { *[Symbol.iterator]() { yield 's1'; yield 's2'; } };
(async () => { for await (const v of syncOnly) print('C: ' + v); print('C: done'); })();

// an async generator with a finally, exited early by break
async function* agf() { try { yield 'x'; yield 'y'; } finally { print('D: finally'); } }
(async () => {
  for await (const v of agf()) { print('D: ' + v); break; }
  print('D: after break');
})();

// manual asyncIterator protocol
const manual = {
  [Symbol.asyncIterator]() {
    let i = 0;
    return { next() { i++; return Promise.resolve(i <= 2 ? { value: 'm' + i, done: false } : { value: undefined, done: true }); } };
  }
};
(async () => { for await (const v of manual) print('E: ' + v); print('E: done'); })();

print('sync end');
