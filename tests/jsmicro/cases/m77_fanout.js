// 1000 reactions on ONE promise: they must all run, in registration order,
// within the same flush. A signal with many subscribers is this shape.
var p = Promise.resolve('V');
var seen = [];
for (var i = 0; i < 1000; i++) (function (n) { p.then(function () { seen.push(n); }); })(i);
p.then(function () {}).then(function () {
  var ordered = true;
  for (var i = 0; i < seen.length; i++) if (seen[i] !== i) { ordered = false; break; }
  print('reactions', seen.length, 'in order', ordered);
});
print('sync');
