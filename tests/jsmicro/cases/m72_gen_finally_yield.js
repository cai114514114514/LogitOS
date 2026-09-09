// A finally block that itself yields during an abrupt close.
async function* g() { try { yield 1; } finally { print('  fin start'); await 0; print('  fin end'); } }
(async function () { for await (const v of g()) break; print('AFTER'); })();
Promise.resolve().then(function(){print('b1');}).then(function(){print('b2');})
  .then(function(){print('b3');}).then(function(){print('b4');})
  .then(function(){print('b5');}).then(function(){print('b6');});
print('sync');
