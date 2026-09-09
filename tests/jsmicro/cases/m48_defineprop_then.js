// Defining `then` as an own data property on a plain object AFTER it has been
// handed to Promise.resolve. Timing of the `then` read is the point.
var o = {};
var p = Promise.resolve().then(function () { return o; });
o.then = function (res) { print('  late then used'); res('LATE'); };
p.then(function (v) { print('got', v); });
print('sync');
