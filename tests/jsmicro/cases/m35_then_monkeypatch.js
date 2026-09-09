// zone.js and every promise-instrumentation library replace
// Promise.prototype.then. The replacement must be on the resolution path too,
// not only on explicit .then calls.
var orig = Promise.prototype.then;
var calls = 0;
Promise.prototype.then = function (a, b) { calls++; return orig.call(this, a, b); };
async function f() { await Promise.resolve(1); return 2; }
f().then(function (v) {
  Promise.prototype.then = orig;
  print('value', v, 'patched-then calls', calls);
});
