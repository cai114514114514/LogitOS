// .finally whose callback returns a thenable must WAIT for it before passing
// the original value through.
Promise.resolve('V').finally(function () {
  print('  fin body');
  return { then: function (res) { print('  fin thenable'); Promise.resolve().then(function(){ res('IGNORED'); }); } };
}).then(function (v) { print('after fin ->', v); });
Promise.resolve().then(function(){print('g1');}).then(function(){print('g2');})
  .then(function(){print('g3');}).then(function(){print('g4');})
  .then(function(){print('g5');}).then(function(){print('g6');});
print('sync');
