// Symbol.species on the promise an async function awaits must not change the
// number of turns.
class Slow extends Promise { static get [Symbol.species]() { return Promise; } }
(async function () { var v = await Slow.resolve('S'); print('awaited', v); })();
Promise.resolve().then(function(){print('d1');}).then(function(){print('d2');})
  .then(function(){print('d3');}).then(function(){print('d4');});
print('sync');
