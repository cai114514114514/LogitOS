// Early exit from for-await must call the async iterator's return() and AWAIT
// its result. A framework that streams and breaks depends on the cleanup.
async function* gen() {
  try { yield 1; yield 2; yield 3; } finally { print('  gen finally'); }
}
(async function () {
  for await (const v of gen()) { print('v', v); if (v === 2) break; }
  print('after loop');
})();
Promise.resolve().then(function(){print('e1');}).then(function(){print('e2');})
  .then(function(){print('e3');}).then(function(){print('e4');})
  .then(function(){print('e5');}).then(function(){print('e6');})
  .then(function(){print('e7');}).then(function(){print('e8');})
  .then(function(){print('e9');}).then(function(){print('e10');});
print('sync');
