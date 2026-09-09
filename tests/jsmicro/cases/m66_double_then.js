// Two reactions on one promise run in registration order, and adding a third
// from inside the first lands after the second.
var p = Promise.resolve('V');
p.then(function (v) { print('r1', v); p.then(function () { print('r3 (added late)'); }); });
p.then(function (v) { print('r2', v); });
print('sync');
