var th = { then: function (res) { print('  th.then'); res(42); } };
async function f() { print('f0'); var v = await th; print('f1', v); }
f();
Promise.resolve().then(function(){print('n1');}).then(function(){print('n2');})
  .then(function(){print('n3');}).then(function(){print('n4');});
print('sync');
