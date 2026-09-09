// Promise.all / allSettled / race / any settle order relative to plain chains.
Promise.all([Promise.resolve(1), 2, Promise.resolve(3)]).then(function(a){print('all', a.join(','));});
Promise.race([Promise.resolve('r1'), Promise.resolve('r2')]).then(function(v){print('race', v);});
Promise.allSettled([Promise.resolve(1), Promise.reject(new Error('x'))]).then(function(a){
  print('allSettled', a.map(function(o){return o.status;}).join(','));
});
Promise.any([Promise.reject(new Error('a')), Promise.resolve('ok')]).then(function(v){print('any', v);});
Promise.resolve().then(function(){print('p1');}).then(function(){print('p2');})
  .then(function(){print('p3');}).then(function(){print('p4');}).then(function(){print('p5');});
print('sync');
