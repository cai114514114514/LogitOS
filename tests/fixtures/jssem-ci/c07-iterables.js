// The iteration protocol itself: who is asked for what, in what order, and
// when return() is called. A virtual-DOM diff spreads children; a store
// iterates a Map; both die differently if this is wrong.
'use strict';

const traced = {
  [Symbol.iterator]() {
    print('iterator requested');
    let i = 0;
    return {
      next() { print('next ' + i); return i < 3 ? { value: i++, done: false } : { value: 'RET', done: true }; },
      return(v) { print('return called with ' + v); return { value: v, done: true }; },
      [Symbol.iterator]() { return this; }
    };
  }
};

print('spread ' + [...traced].join(','));
print('--');
for (const v of traced) { print('loop ' + v); if (v === 1) break; }
print('--');
const [a, b] = traced;
print('destructure ' + a + ' ' + b);
print('--');
print('Array.from ' + Array.from(traced).join(','));
print('--');

// a `done: true` result's value is NOT part of the iteration
function* r() { yield 1; return 99; }
print('return value dropped ' + [...r()].join(','));

// String, Map, Set iteration order and shape
print('string ' + [...'abé'].join('|'));
const m = new Map([['k1', 1], ['k2', 2]]);
print('map ' + [...m].map(p => p[0] + '=' + p[1]).join(','));
print('map keys ' + [...m.keys()].join(','));
print('set ' + [...new Set([3, 1, 3, 2])].join(','));
print('entries ' + [...Object.entries({ x: 1, y: 2 })].map(p => p.join(':')).join(','));

// Array destructuring from a Map inside for-of (the ubiquitous store pattern)
for (const [k, v] of m) print('mapentry ' + k + ' ' + v);

// a throwing body still calls return()
try {
  for (const v of traced) { print('throwing at ' + v); throw new Error('x'); }
} catch (err) { print('caught ' + err.constructor.name); }
print('--');

// an iterable whose Symbol.iterator is not callable
try { [...{ [Symbol.iterator]: 5 }]; } catch (err) { print('bad iterator ' + err.constructor.name); }
try { [...{}]; } catch (err) { print('not iterable ' + err.constructor.name); }

// array holes are NOT skipped by iteration but ARE by forEach
const holey = [1, , 3];
print('holes iterate ' + [...holey].map(String).join(','));
let seen = []; holey.forEach(v => seen.push(v));
print('holes forEach ' + seen.join(','));
print('in ' + (1 in holey));
