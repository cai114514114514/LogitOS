// .finally inserts a turn and must pass the value through unchanged.
Promise.resolve('V').finally(function(){print('fin');}).then(function(v){print('after fin', v);});
Promise.reject(new Error('E')).finally(function(){print('finR');}).catch(function(e){print('after finR', e.message);});
Promise.resolve().then(function(){print('c1');}).then(function(){print('c2');})
  .then(function(){print('c3');}).then(function(){print('c4');});
print('sync');
