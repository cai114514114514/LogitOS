// 5000 chained thens. A framework's reactive graph is a chain of exactly this
// shape; an engine that recurses instead of queueing overflows here.
var p = Promise.resolve(0);
for (var i = 0; i < 5000; i++) p = p.then(function (n) { return n + 1; });
p.then(function (n) { print('depth', n); }, function (e) { print('THREW', e.name); });
