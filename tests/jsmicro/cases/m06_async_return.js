// `return p` from an async function costs TWO extra turns over `return v`.
async function ret_v() { return 'v'; }
async function ret_p() { return Promise.resolve('p'); }
ret_v().then(function(x){print('resolved', x);});
ret_p().then(function(x){print('resolved', x);});
Promise.resolve().then(function(){print('k1');}).then(function(){print('k2');})
  .then(function(){print('k3');}).then(function(){print('k4');});
print('sync');
