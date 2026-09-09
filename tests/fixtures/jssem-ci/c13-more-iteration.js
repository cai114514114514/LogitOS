// The rest of the iteration protocol, and the class-field arrow that every
// React class component is made of.
'use strict';

// a class field holding an arrow captures the instance's `this` at
// construction -- the standard event-handler idiom
class Comp {
  state = 'S';
  handle = () => this.state;
  static make() { return new Comp(); }
}
const inst = Comp.make();
const detached = inst.handle;
print('arrow field ' + detached());
print('own not proto ' + Object.prototype.hasOwnProperty.call(inst, 'handle') + ' ' +
      ('handle' in Object.getPrototypeOf(inst)));

// yield* to an object that is missing throw(): the delegate must be closed
// and a TypeError raised
function* del() { yield* { [Symbol.iterator]() { return { next: () => ({ value: 1, done: false }),
                                                          return() { print('delegate return'); return { done: true }; } }; } }; }
const d = del();
print('del ' + d.next().value);
try { d.throw(new Error('t')); } catch (e) { print('del throw ' + e.constructor.name); }

// yield inside a finally, reached by .return()
function* yf() {
  try { yield 'a'; }
  finally { yield 'from-finally'; print('yf finally done'); }
}
const y = yf();
print('yf ' + y.next().value);
print('yf ' + JSON.stringify(y.return('R')));
print('yf ' + JSON.stringify(y.next()));

// an iterator whose return() throws during a break
const badClose = {
  [Symbol.iterator]() {
    return { next: () => ({ value: 1, done: false }),
             return() { throw new Error('close failed'); } };
  }
};
try { for (const v of badClose) { void v; break; } print('badClose no throw'); }
catch (e) { print('badClose ' + e.constructor.name + ' ' + e.message); }

// destructuring fewer elements than the iterator yields closes it
const closer = {
  [Symbol.iterator]() { let i = 0; return { next: () => ({ value: i++, done: false }),
                                            return() { print('destructure close'); return { done: true }; } }; }
};
const [z0, z1] = closer;
print('short destructure ' + z0 + ' ' + z1);

// Array.from with a mapper, and its `this`
print('from map ' + Array.from({ length: 3 }, (_, i) => i * 2).join(','));
print('from set ' + Array.from(new Set([1, 1, 2])).join(','));
print('from string ' + Array.from('ab').join(','));

// spread into a function call uses the iterator, not length
function sum(...xs) { return xs.length + ':' + xs.join('+'); }
print('spread call ' + sum(...new Set([5, 6, 7])));
print('spread mixed ' + sum(1, ...[2, 3], 4));

// spread into an object literal is NOT iteration -- it is own enumerable copy
print('object spread ' + JSON.stringify({ ...{ a: 1 }, ...'xy', ...[9] }));
print('object spread null ' + JSON.stringify({ ...null, ...undefined, k: 1 }));

// iterator helpers (ES2025). Absent is a finding but a mild one: bundlers do
// not emit these yet. Present-and-wrong would be worse.
print('helpers ' + ['map', 'filter', 'take', 'drop', 'toArray']
      .map(n => n + '=' + (typeof (function* () {})()[n])).join(' '));

// Symbol.iterator inherited through two levels of class
class It0 { *[Symbol.iterator]() { yield 1; yield 2; } }
class It1 extends It0 {}
class It2 extends It1 { *[Symbol.iterator]() { yield 0; yield* super[Symbol.iterator](); } }
print('inherited iter ' + [...new It2()].join(','));

// for-of over `arguments`
function argsOf() { return [...arguments].join(','); }
print('arguments ' + argsOf('a', 'b'));

// a for-of whose binding is a destructuring pattern with a hole and a default
for (const [, b = 'B'] of [[1], [1, 2]]) print('pat ' + b);

// entries()/keys()/values() on an array, and their toStringTag
const ai = [10, 20].entries();
print('array iter ' + Object.prototype.toString.call(ai));
print('array entries ' + [...ai].map(p => p.join(':')).join(','));
