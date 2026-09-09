// Overwriting Promise.prototype.constructor changes which constructor .then
// uses to build its result. Instrumentation libraries do this.
function Fake(exec) { var p = new Promise(exec); Object.setPrototypeOf(p, Fake.prototype); return p; }
Fake.prototype = Object.create(Promise.prototype);
Fake[Symbol.species] = Fake;
var p = Promise.resolve(1);
Object.defineProperty(p, 'constructor', { value: Fake, configurable: true });
var q = p.then(function () {});
print('result is Fake?', q instanceof Fake, '| is Promise?', q instanceof Promise);
