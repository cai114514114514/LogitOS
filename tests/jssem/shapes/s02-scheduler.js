// s02 -- the batching scheduler. Every framework in the survey coalesces
// updates by scheduling ONE microtask and assuming it runs after the current
// synchronous block and before anything else. That assumption is an ORDER, and
// an order is a string, so node decides it.
//
// The cases below are the four scheduler behaviours a framework actually
// depends on, and each has a way to be wrong that still "works" in a demo:
//   1. coalescing        -- N writes in one turn produce ONE flush
//   2. re-entrancy       -- scheduling from inside a flush produces a SECOND
//                           flush in the same drain, not a lost update
//   3. relative order    -- a microtask queued before a promise reaction runs
//                           before it, and queueMicrotask must not cost a turn
//                           that .then() does not
//   4. await interleave  -- an async function's continuation is a microtask and
//                           takes its place in the same queue

var out = [];
function t(s) { out.push(s); }

// 1. coalescing
var scheduled = false, writes = 0;
function write(n) {
  writes++;
  if (scheduled) return;
  scheduled = true;
  Promise.resolve().then(function () {
    scheduled = false;
    t('flush:' + writes);
    writes = 0;
  });
}
write(1); write(2); write(3);
t('sync-after-writes');

// 2. re-entrancy: a flush that schedules again
var depth = 0;
function reflush() {
  Promise.resolve().then(function () {
    depth++;
    t('reflush:' + depth);
    if (depth < 3) reflush();
  });
}
reflush();

// 3. queueMicrotask vs .then, in the order they were queued
queueMicrotask(function () { t('qmt-A'); });
Promise.resolve().then(function () { t('then-B'); });
queueMicrotask(function () { t('qmt-C'); });

// 4. await interleaving with plain reactions
(async function () {
  t('async-sync-part');
  await null;
  t('after-await-1');
  await null;
  t('after-await-2');
})();
Promise.resolve().then(function () { t('then-D'); });

t('end-of-script');

// Print the accumulated order after the queue has fully drained. Chaining this
// many .then()s is how the case reaches "after everything" without a timer --
// there is no setTimeout in this runner and a macrotask would be a different
// question anyway.
var p = Promise.resolve();
for (var i = 0; i < 12; i++) p = p.then(function () {});
p.then(function () { print('ORDER =', out.join(' ')); });
