// Destructuring in every shape a transpiler emits for props/options objects.
'use strict';

function d(name) { print('default ' + name); return name; }

// defaults are evaluated LAZILY and LEFT TO RIGHT, only for undefined
const { p = d('p'), q = d('q'), r = d('r') } = { q: 'given', r: null };
print('obj ' + p + ' ' + q + ' ' + r);

// earlier bindings are visible to later defaults
const { x = 1, y = x + 1, z = y + 1 } = {};
print('chain ' + x + ' ' + y + ' ' + z);

// nested + rename + computed key + rest
const key = 'dyn';
const { a: { b: renamed = 'nb' } = {}, [key]: computed, ...rest } = { a: {}, dyn: 'D', e: 1, f: 2 };
print('nested ' + renamed + ' ' + computed + ' ' + JSON.stringify(rest));

// array patterns: holes, defaults, rest, nested
const [, second = 'S', ...tail] = ['one', undefined, 3, 4];
print('arr ' + second + ' ' + tail.join(','));
const [[i1, i2] = [7, 8]] = [];
print('nested arr ' + i1 + ' ' + i2);

// parameter destructuring with a whole-object default
function f({ m = 1, n = m * 2 } = {}, [o = 'O'] = []) { return [m, n, o].join(','); }
print('params ' + f());
print('params ' + f({ m: 5 }, ['P']));

// assignment (not declaration) patterns need parens; a compiler emits these
let g1, g2, g3 = {};
({ g1, h: g2 = 'H', ...g3 } = { g1: 'G', k: 1 });
print('assign ' + g1 + ' ' + g2 + ' ' + JSON.stringify(g3));
let s1, s2;
[s1, s2] = [s2, s1] = ['A', 'B'];
print('swapchain ' + s1 + ' ' + s2);

// destructuring null/undefined throws TypeError; destructuring a primitive
// boxes it
try { const { u } = null; void u; } catch (err) { print('null ' + err.constructor.name); }
const { length } = 'hello';
print('boxed ' + length);
const { toFixed } = 5;
print('boxed fn ' + (typeof toFixed));

// object rest copies own ENUMERABLE properties only, and does not copy getters
const src = Object.defineProperties({}, {
  vis: { value: 1, enumerable: true },
  hid: { value: 2, enumerable: false },
  got: { get() { print('getter ran'); return 3; }, enumerable: true }
});
const { ...copied } = src;
print('rest ' + JSON.stringify(copied));
print('rest desc ' + JSON.stringify(Object.getOwnPropertyDescriptor(copied, 'got')));

// for-of over entries with destructuring defaults
for (const [k, v = 'DEF'] of [['a', 1], ['b']]) print('entry ' + k + ' ' + v);
