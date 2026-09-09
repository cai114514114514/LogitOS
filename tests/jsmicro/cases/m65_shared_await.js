// Three async functions awaiting the SAME promise resume in the order they
// attached. A framework awaiting one shared "ready" promise depends on it.
var res;
var gate = new Promise(function (r) { res = r; });
async function a(tag) { await gate; print('resumed', tag); }
a('one'); a('two'); a('three');
gate.then(function () { print('plain then'); });
res('go');
Promise.resolve().then(function(){print('m1');}).then(function(){print('m2');})
  .then(function(){print('m3');});
print('sync');
