// Promise.resolve(p) must return p itself when p is a native promise, and must
// wrap when it is a subclass or a thenable. Frameworks rely on the identity to
// avoid a tick.
var p = Promise.resolve(1);
print('identity', Promise.resolve(p) === p);
class MyP extends Promise {}
var q = MyP.resolve(2);
print('subclass ctor', q.constructor === MyP);
print('subclass identity', MyP.resolve(q) === q);
print('cross', Promise.resolve(q) === q);
var th = { then: function(r){ r(3); } };
print('thenable identity', Promise.resolve(th) === th);
