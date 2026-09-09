// MINIMAL: an abrupt exit from `for await` must AWAIT the iterator's return()
// before the statement after the loop runs (AsyncIteratorClose step 7,
// "? Await(innerResult)").
async function* gen(tag) { try { yield 1; yield 2; } finally { print('CLEANUP-' + tag); } }
(async function () {
  for await (const v of gen('break')) break;
  print('AFTER-break');
  try { for await (const v of gen('throw')) throw new Error('x'); } catch (e) {}
  print('AFTER-throw');
})();
