// Symbol.species on a Promise subclass decides what .then returns. zone.js,
// bluebird interop and every "cancellable promise" wrapper stand on this.
class P1 extends Promise {}
var a = P1.resolve(1);
print('a is P1', a instanceof P1);
var b = a.then(function(){});
print('then result is P1', b instanceof P1);
class P2 extends Promise { static get [Symbol.species]() { return Promise; } }
var c = P2.resolve(1);
var d = c.then(function(){});
print('species=Promise -> P2?', d instanceof P2, 'Promise?', d instanceof Promise);
print('finally species', P1.resolve(1).finally(function(){}) instanceof P1);
print('catch species', P1.resolve(1).catch(function(){}) instanceof P1);
