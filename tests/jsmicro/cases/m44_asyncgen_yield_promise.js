// `yield p` inside an async generator AWAITS p before delivering. That extra
// turn is what a streaming consumer's ordering rests on.
async function* g() { yield Promise.resolve('P'); yield 'V'; }
(async function () { for await (const v of g()) print('got', v); print('done'); })();
Promise.resolve().then(function(){print('u1');}).then(function(){print('u2');})
  .then(function(){print('u3');}).then(function(){print('u4');})
  .then(function(){print('u5');}).then(function(){print('u6');})
  .then(function(){print('u7');}).then(function(){print('u8');});
print('sync');
