// await inside try/finally, and an await in the finally block itself.
async function f() {
  try { print('try'); await 0; print('after await in try'); return 'R'; }
  finally { print('finally'); await 0; print('after await in finally'); }
}
f().then(function (v) { print('resolved', v); });
Promise.resolve().then(function(){print('w1');}).then(function(){print('w2');})
  .then(function(){print('w3');}).then(function(){print('w4');})
  .then(function(){print('w5');}).then(function(){print('w6');});
print('sync');
