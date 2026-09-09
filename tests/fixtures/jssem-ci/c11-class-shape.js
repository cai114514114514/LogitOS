// The observable SHAPE of a class: descriptors, name, length, the prototype
// chain, and the well-known symbols a class may define. A framework that
// walks a component's prototype (Angular DI, MobX makeObservable, any
// decorator) reads exactly these.
'use strict';

class C {
  constructor(a, b) { void a; void b; }
  m(x) { return x; }
  get g() { return 1; }
  set g(v) { void v; }
  static s() { return 2; }
  *gen() { yield 1; }
  async am() { return 3; }
  async *ag() { yield 4; }
  [Symbol.iterator]() { let i = 0; return { next: () => ({ value: i, done: i++ >= 2 }) }; }
  get [Symbol.toStringTag]() { return 'CTag'; }
}

const d = Object.getOwnPropertyDescriptor(C.prototype, 'm');
print('method desc ' + JSON.stringify({ w: d.writable, e: d.enumerable, c: d.configurable }));
const dg = Object.getOwnPropertyDescriptor(C.prototype, 'g');
print('accessor desc ' + JSON.stringify({ get: typeof dg.get, set: typeof dg.set, e: dg.enumerable, c: dg.configurable }));
const dc = Object.getOwnPropertyDescriptor(C.prototype, 'constructor');
print('ctor desc ' + JSON.stringify({ w: dc.writable, e: dc.enumerable, c: dc.configurable }));

print('proto keys ' + Object.getOwnPropertyNames(C.prototype).join(','));
print('proto syms ' + Object.getOwnPropertySymbols(C.prototype).map(String).join(','));
print('static keys ' + Object.getOwnPropertyNames(C).join(','));

print('names ' + C.name + ' ' + C.prototype.m.name + ' ' + dg.get.name + ' ' + dg.set.name);
print('lengths ' + C.length + ' ' + C.prototype.m.length);
print('tag ' + Object.prototype.toString.call(new C()));
print('iterate ' + [...new C()].join(','));

// a class is not callable without new, and methods are not constructible
try { C(); } catch (e) { print('call class ' + e.constructor.name); }
try { new (C.prototype.m)(); } catch (e) { print('new method ' + e.constructor.name); }
try { new (C.prototype.gen)(); } catch (e) { print('new gen ' + e.constructor.name); }

// class body is strict: assigning to a frozen thing throws
class Strict { m() { const o = Object.freeze({}); try { o.x = 1; return 'no throw'; } catch (e) { return e.constructor.name; } } }
print('strict body ' + new Strict().m());

// extends null, extends an expression, extends a bound function
class Null extends null {}
print('extends null proto ' + (Object.getPrototypeOf(Null.prototype) === null) + ' ' + (Object.getPrototypeOf(Null) === Function.prototype));
try { new Null(); print('new Null ok'); } catch (e) { print('new Null ' + e.constructor.name); }

function mixin(B) { return class extends B { extra() { return 'extra'; } }; }
class Root { root() { return 'root'; } }
const M = mixin(Root);
const mi = new M();
print('mixin ' + mi.root() + ' ' + mi.extra() + ' ' + (mi instanceof Root));

// the static side inherits from the base constructor
print('static inherit ' + (Object.getPrototypeOf(M) === Root) + ' ' + (typeof M.name));

// Symbol.hasInstance defined (not assigned -- assignment fails in both
// engines because the inherited property is non-writable, and a hand-written
// expectation would have called that a bug)
class H {}
Object.defineProperty(H, Symbol.hasInstance, { value: (x) => x === 42 });
print('hasInstance ' + (42 instanceof H) + ' ' + (new H() instanceof H));

// Symbol.species through a subclass of Array
class MyArr extends Array { static get [Symbol.species]() { return Array; } }
const ma = MyArr.from([1, 2, 3]);
print('species ' + (ma.map(x => x) instanceof MyArr) + ' ' + (ma.map(x => x) instanceof Array) + ' ' + ma.length);

// Reflect.construct with a different new.target
class P1 { constructor() { this.who = new.target.name; } }
class P2 extends P1 {}
print('reflect ' + Reflect.construct(P1, [], P2).who);

// a getter defined on a prototype AFTER instances exist is still seen
class Late {}
const li = new Late();
Object.defineProperty(Late.prototype, 'v', { get() { return 'late'; }, configurable: true });
print('late accessor ' + li.v);
