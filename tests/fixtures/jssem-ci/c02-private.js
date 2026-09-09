// Private names. Bundlers emit these directly now (esbuild/swc keep #x when
// the target is modern), and a framework base class uses them for internal
// state it must hide from a subclass.
'use strict';

class C {
  #x = 1;
  static #count = 0;
  #inc() { this.#x++; return this.#x; }
  get #dbl() { return this.#x * 2; }
  set #dbl(v) { this.#x = v / 2; }
  static #make() { C.#count++; return new C(); }

  constructor() { C.#count++; }
  peek() { return this.#x; }
  bump() { return this.#inc(); }
  dbl() { return this.#dbl; }
  setDbl(v) { this.#dbl = v; return this.#x; }
  static count() { return C.#count; }
  static make() { return C.#make(); }
  static has(o) { return #x in o; }
}

const c = new C();
print('peek ' + c.peek());
print('bump ' + c.bump());
print('dbl ' + c.dbl());
print('setDbl ' + c.setDbl(20));
print('count ' + C.count());
C.make();
print('count2 ' + C.count());

// the brand check -- `#x in o` is the ergonomic-brand-check proposal and is
// how a library tells "one of mine" from "looks like one of mine"
print('has(c) ' + C.has(c));
print('has({}) ' + C.has({}));
print('has(null-proto) ' + C.has(Object.create(null)));

// a private field is not enumerable, not on Object.keys, not in JSON
print('keys ' + JSON.stringify(Object.keys(c)));
print('json ' + JSON.stringify(c));
print('getOwn ' + Object.getOwnPropertyNames(c).length);

// accessing an absent private name throws TypeError
try { C.has.call(null, {}); } catch (e) { print('call-null ' + e.constructor.name); }
class D { #y = 1; static read(o) { return o.#y; } }
try { D.read({}); } catch (e) { print('absent-private ' + e.constructor.name); }

// private names in a subclass do not collide with the base's same spelling
class E { #x = 'E'; ex() { return this.#x; } }
class F extends E { #x = 'F'; fx() { return this.#x; } }
const f = new F();
print('shadow ' + f.ex() + ' ' + f.fx());
