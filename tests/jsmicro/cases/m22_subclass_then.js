// A subclass that overrides `then` -- the resolution path must go through the
// override. This is exactly the hook a promise-instrumentation library uses.
class Traced extends Promise {
  then(a, b) { print('  Traced.then'); return super.then(a, b); }
}
Traced.resolve('x').then(function (v) { print('got', v); });
Promise.resolve().then(function(){print('s1');}).then(function(){print('s2');})
  .then(function(){print('s3');});
print('sync');
