// for await over an object with only Symbol.iterator whose values are
// PROMISES: each value is awaited, one extra turn per element.
var arr = [Promise.resolve('a'), 'b', Promise.resolve('c')];
(async function () { for await (const v of arr) print('v', v); print('done'); })();
Promise.resolve().then(function(){print('j1');}).then(function(){print('j2');})
  .then(function(){print('j3');}).then(function(){print('j4');})
  .then(function(){print('j5');}).then(function(){print('j6');})
  .then(function(){print('j7');}).then(function(){print('j8');});
print('sync');
