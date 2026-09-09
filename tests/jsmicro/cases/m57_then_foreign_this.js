// Promise.prototype.then called with a non-promise `this` must throw a
// TypeError. Framework feature-tests do exactly this.
try { Promise.prototype.then.call({}, function () {}); print('WRONG: no throw'); }
catch (e) { print('then on plain object ->', e.constructor.name); }
try { print('resolve on plain ctor ->', Promise.resolve.call(function () {}, 1) !== undefined); }
catch (e) { print('resolve.call ->', e.constructor.name); }
print('toStringTag', Promise.prototype[Symbol.toStringTag]);
print('tostring', Object.prototype.toString.call(Promise.resolve()));
