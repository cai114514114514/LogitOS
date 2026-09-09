// The corners of the same area: super assignment receivers, re-entrancy,
// astral iteration, __proto__ in an object literal, and the observable
// side-effect ORDER of destructuring (how many times a getter runs).
'use strict';

// super.x = v assigns on the RECEIVER, not on the home object's prototype
class SB { }
SB.prototype.p = 'proto';
class SD extends SB { set() { super.p = 'set-via-super'; } }
const sd = new SD();
sd.set();
print('super set own ' + Object.prototype.hasOwnProperty.call(sd, 'p') + ' ' + sd.p + ' ' + SB.prototype.p);

// a setter on the base is invoked with the derived instance as receiver
class SB2 { set q(v) { print('setter this-is-instance ' + (this === sd2) + ' ' + v); } }
class SD2 extends SB2 { go() { super.q = 'V'; } }
const sd2 = new SD2();
sd2.go();

// generator re-entrancy: calling next() from inside the generator
function* re() { try { it.next(); } catch (e) { print('reentrant ' + e.constructor.name); } yield 1; }
const it = re();
print('re ' + it.next().value);

// astral code points: iteration is by CODE POINT, .length is by code unit
const emoji = 'a\u{1F600}b';
print('astral length ' + emoji.length);
print('astral iterate ' + [...emoji].length + ' ' + [...emoji].map(c => c.codePointAt(0).toString(16)).join(','));
print('astral for-of ' + (() => { let n = 0; for (const _ of emoji) n++; return n; })());
print('astral at ' + emoji.at(-1) + ' ' + emoji.codePointAt(1).toString(16));
print('lone surrogate ' + [...'\uD83D'].length + ' ' + JSON.stringify([...'\uD83D'][0]));

// __proto__ in an object literal SETS the prototype; a computed or shorthand
// key of the same name defines an ordinary property
const base = { tag: 'B' };
const o1 = { __proto__: base };
const o2 = { ['__proto__']: base };
const __proto__ = base;
const o3 = { __proto__ };
print('proto literal ' + (Object.getPrototypeOf(o1) === base) + ' ' + o1.tag);
print('proto computed ' + (Object.getPrototypeOf(o2) === base) + ' ' + Object.prototype.hasOwnProperty.call(o2, '__proto__'));
print('proto shorthand ' + (Object.getPrototypeOf(o3) === base) + ' ' + Object.prototype.hasOwnProperty.call(o3, '__proto__'));

// destructuring reads each source property EXACTLY ONCE, in pattern order
let reads = [];
const src = { get a() { reads.push('a'); return 1; }, get b() { reads.push('b'); return undefined; }, get c() { reads.push('c'); return 3; } };
const { c: cc, a: aa, b: bb = 'D' } = src;
print('read order ' + reads.join(',') + ' -> ' + aa + ' ' + bb + ' ' + cc);

// a computed key in a pattern is evaluated in pattern order too
reads = [];
function k(n) { reads.push('key' + n); return 'a'; }
const { [k(1)]: v1, [k(2)]: v2 } = { a: 'A' };
print('computed key order ' + reads.join(',') + ' -> ' + v1 + ' ' + v2);

// class private static read through a subclass instance is a TypeError
class PS { static #s = 1; static read(o) { return o.#s; } }
class PSub extends PS { }
try { print('privstatic ' + PS.read(PSub)); } catch (e) { print('privstatic ' + e.constructor.name); }

// new.target inside an arrow inside a constructor
class NTC { constructor() { const f = () => new.target; print('arrow new.target ' + (f() === NTC)); } }
new NTC();

// a getter/setter pair split across a class and its prototype patch
class Split { get v() { return 'get'; } }
Object.defineProperty(Split.prototype, 'v', { set(x) { print('replaced by set ' + x); }, configurable: true });
const sp = new Split();
print('split get ' + sp.v);
sp.v = 1;

// replacing Array.prototype[Symbol.iterator] changes spread of an array
const realIter = Array.prototype[Symbol.iterator];
Array.prototype[Symbol.iterator] = function* () { yield 'HIJACK'; };
print('hijacked spread ' + [...[1, 2, 3]].join(','));
Array.prototype[Symbol.iterator] = realIter;
print('restored spread ' + [...[1, 2, 3]].join(','));

// Array.from over a holey array and an array-like with a length getter
print('from holes ' + JSON.stringify(Array.from([1, , 3])));
print('from arraylike ' + Array.from({ 0: 'x', 1: 'y', get length() { print('length read'); return 2; } }).join(','));

// a class extending a function that is not a constructor
try { class Bad extends (() => {}) { } void Bad; print('extends arrow ok'); }
catch (e) { print('extends arrow ' + e.constructor.name); }

// function declarations inside a labelled block (Annex B / strict interaction)
{ function inBlock() { return 'block-fn'; } print('block fn ' + inBlock()); }
print('block fn outside ' + (typeof inBlock));
