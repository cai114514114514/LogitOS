// Resolving a promise WITH a promise costs two extra turns (the adoption goes
// through NewPromiseResolveThenableJob then the inner .then). Count them.
var res;
var outer = new Promise(function (r) { res = r; });
outer.then(function (v) { print('outer settled', v); });
res(Promise.resolve('inner'));
Promise.resolve().then(function(){print('i1');}).then(function(){print('i2');})
  .then(function(){print('i3');}).then(function(){print('i4');})
  .then(function(){print('i5');});
print('sync');
