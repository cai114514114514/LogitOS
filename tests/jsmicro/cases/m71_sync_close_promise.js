// for await over a SYNC iterator whose return() gives a PROMISE.
// CreateAsyncFromSyncIterator's return step awaits it; m33's did not need to.
var it = {
  [Symbol.iterator]() { return this; },
  next() { return { value: 1, done: false }; },
  return() { print('sync return() called'); return { value: Promise.resolve().then(function () { print('  inner settled'); }), done: true }; }
};
(async function () { for await (const v of it) break; print('AFTER'); })();
