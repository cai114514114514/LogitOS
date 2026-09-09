// Non-callable arguments to .then must PASS THROUGH, value and rejection alike.
Promise.resolve('V').then(null).then(undefined).then(1).then(function(v){print('through', v);});
Promise.reject('R').then(function(){print('WRONG');}).then(null, null).catch(function(e){print('rthrough', e);});
