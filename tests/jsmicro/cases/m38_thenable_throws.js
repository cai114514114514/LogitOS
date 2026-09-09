// A thenable whose then() throws AFTER resolving must be ignored; one that
// throws BEFORE must reject.
var a = { then: function (res) { res('A'); throw new Error('late'); } };
Promise.resolve(a).then(function (v) { print('a ->', v); }, function (e) { print('a rejected', e.message); });
var b = { then: function (res) { throw new Error('early'); } };
Promise.resolve(b).then(function (v) { print('b ->', v); }, function (e) { print('b rejected', e.message); });
var c = { get then() { throw new Error('getter'); } };
Promise.resolve(c).then(function (v) { print('c ->', v); }, function (e) { print('c rejected', e.message); });
