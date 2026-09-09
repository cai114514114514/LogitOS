// Two chains of different length started at the same time. The interleave is
// the whole of "does .then schedule one tick at a time".
Promise.resolve().then(function(){print('a1');}).then(function(){print('a2');}).then(function(){print('a3');});
Promise.resolve().then(function(){print('b1');}).then(function(){print('b2');}).then(function(){print('b3');});
print('sync');
