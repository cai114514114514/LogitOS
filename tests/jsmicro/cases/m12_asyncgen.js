// for await..of over an async generator: the resumption order is what a
// streaming framework's backpressure rests on.
async function* gen() { print('g A'); yield 1; print('g B'); yield 2; print('g C'); }
(async function () {
  for await (const v of gen()) print('got', v);
  print('loop done');
})();
Promise.resolve().then(function(){print('x1');}).then(function(){print('x2');})
  .then(function(){print('x3');}).then(function(){print('x4');})
  .then(function(){print('x5');}).then(function(){print('x6');})
  .then(function(){print('x7');}).then(function(){print('x8');});
print('sync');
