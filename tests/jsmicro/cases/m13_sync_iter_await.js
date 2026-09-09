// for await over a SYNC iterable wraps each value: a different tick count from
// the async-iterator path.
(async function () {
  for await (const v of [1, 2, 3]) print('v', v);
  print('done');
})();
Promise.resolve().then(function(){print('y1');}).then(function(){print('y2');})
  .then(function(){print('y3');}).then(function(){print('y4');})
  .then(function(){print('y5');}).then(function(){print('y6');});
print('sync');
