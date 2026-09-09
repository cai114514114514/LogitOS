// Rejection propagates through the same queue as resolution.
Promise.reject(new Error('r')).catch(function(e){print('caught', e.message);}).then(function(){print('after catch');});
Promise.resolve().then(function(){ throw new Error('thrown'); }).catch(function(e){print('caught2', e.message);});
Promise.resolve().then(function(){print('z1');}).then(function(){print('z2');})
  .then(function(){print('z3');});
print('sync');
