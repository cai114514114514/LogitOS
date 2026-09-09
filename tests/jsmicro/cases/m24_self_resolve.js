// Resolving a promise with itself is a TypeError, delivered as a rejection.
var res;
var p = new Promise(function (r) { res = r; });
p.catch(function (e) { print('self-resolve ->', e.constructor.name, e.message.length > 0); });
res(p);
