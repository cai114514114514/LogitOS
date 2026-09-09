// asyncGenerator.return(v) must await v before delivering, and must await the
// generator's own finally. Two Awaits the spec names separately.
async function* g() { try { yield 1; } finally { print('  finally'); } }
var it = g();
it.next().then(function () { return it.return(Promise.resolve('R')); })
         .then(function (r) { print('return ->', r.value, r.done); });
Promise.resolve().then(function(){print('a1');}).then(function(){print('a2');})
  .then(function(){print('a3');}).then(function(){print('a4');})
  .then(function(){print('a5');}).then(function(){print('a6');})
  .then(function(){print('a7');}).then(function(){print('a8');});
print('sync');
