// Whether a rejection with no handler is OBSERVABLE at all. A handler attached
// in a LATER turn must still receive it (a rejection is not lost by being late).
var p = Promise.reject(new Error('late-handled'));
Promise.resolve().then(function () {}).then(function () {}).then(function () {
  p.catch(function (e) { print('caught late:', e.message); });
});
Promise.resolve().then(function(){print('t1');}).then(function(){print('t2');})
  .then(function(){print('t3');}).then(function(){print('t4');})
  .then(function(){print('t5');});
