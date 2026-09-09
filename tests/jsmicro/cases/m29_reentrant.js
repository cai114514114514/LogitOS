// Resolving from inside a reaction, and attaching a reaction to an
// already-settled promise from inside another reaction.
var res;
var gate = new Promise(function (r) { res = r; });
gate.then(function (v) { print('gate', v); });
Promise.resolve().then(function () {
  print('r1');
  res('opened');
  Promise.resolve().then(function () { print('nested'); });
}).then(function () { print('r2'); }).then(function () { print('r3'); });
print('sync');
