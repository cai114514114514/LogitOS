// How many turns does `await v` cost for a plain value, and for a native
// promise? The spec changed this in 2018 (await of a native promise is 1 tick,
// not 3). An engine on the old rule reorders every framework's flush.
async function f() { print('f start'); await 0; print('f after await 0'); }
async function g() { print('g start'); await Promise.resolve(); print('g after await P'); }
f(); g();
Promise.resolve().then(function(){print('m1');}).then(function(){print('m2');})
  .then(function(){print('m3');}).then(function(){print('m4');})
  .then(function(){print('m5');}).then(function(){print('m6');});
print('sync end');
