// await inside a loop with continue/break through try/finally.
(async function () {
  for (var i = 0; i < 4; i++) {
    try { if (i === 1) continue; if (i === 3) break; await i; print('body', i); }
    finally { print('  fin', i); }
  }
  print('after');
})();
Promise.resolve().then(function(){print('k1');}).then(function(){print('k2');})
  .then(function(){print('k3');}).then(function(){print('k4');})
  .then(function(){print('k5');}).then(function(){print('k6');});
print('sync');
