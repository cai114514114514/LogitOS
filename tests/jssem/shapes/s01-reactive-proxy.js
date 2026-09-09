// s01 -- the dependency-tracking Proxy, which is the mechanism under every
// modern reactive framework: read through a `get` trap to record a dependency,
// write through `set`/`defineProperty` to invalidate it, and batch the
// invalidations into ONE microtask.
//
// NOTHING HERE NAMES A FRAMEWORK and nothing branches on one. It is the
// mechanism written out in the open, because the corpus in tests/jsfb is
// entirely hand-written DOM and compiler-emitted DOM -- neither of which uses
// this shape -- so the matrix cannot answer whether the engine supports it.
// This can, and it is a program that PRINTS, so node decides.
//
// The array half is deliberate and is the expensive part: an Array's `length`
// is non-configurable and writable, so every mutator that updates it goes
// through [[DefineOwnProperty]] on the proxy, which is where the post-trap
// invariant lives.

var log = [];
var pending = null;
var effects = [];

function schedule(fn) {
  effects.push(fn);
  if (pending) return;
  pending = Promise.resolve().then(function () {
    pending = null;
    var run = effects; effects = [];
    log.push('flush(' + run.length + ')');
    for (var i = 0; i < run.length; i++) run[i]();
  });
}

function reactive(target, name) {
  return new Proxy(target, {
    get: function (t, k, r) { return Reflect.get(t, k, r); },
    set: function (t, k, v, r) {
      var ok = Reflect.set(t, k, v, r);
      schedule(function () { log.push('effect:' + name + '.' + String(k)); });
      return ok;
    },
    defineProperty: function (t, k, d) { return Reflect.defineProperty(t, k, d); },
    deleteProperty: function (t, k) { return Reflect.deleteProperty(t, k); },
    has: function (t, k) { return Reflect.has(t, k); },
    ownKeys: function (t) { return Reflect.ownKeys(t); },
    getOwnPropertyDescriptor: function (t, k) { return Reflect.getOwnPropertyDescriptor(t, k); }
  });
}

// --- plain object -----------------------------------------------------------
var state = reactive({ a: 1, b: 2 }, 'state');
print('read a =', state.a);
state.a = 10;
state.b = 20;
print('sync after writes =', state.a + ',' + state.b);
print('log so far =', log.join('|'));

// --- array, the half that exercises the length invariant --------------------
var list = reactive([1, 2, 3], 'list');
var mut = [];
function attempt(tag, fn) {
  try { fn(); mut.push(tag + ':ok'); }
  catch (e) { mut.push(tag + ':' + e.constructor.name); }
}
attempt('push', function () { list.push(4); });
attempt('pop', function () { list.pop(); });
attempt('shift', function () { list.shift(); });
attempt('unshift', function () { list.unshift(0); });
attempt('splice', function () { list.splice(1, 1); });
attempt('lenset', function () { list.length = 1; });
attempt('index', function () { list[0] = 99; });
print('array mutators =', mut.join(' '));
print('array contents =', Array.prototype.join.call(list, ','));

// --- has / delete / enumerate through the traps -----------------------------
print('in operator =', ('a' in state) + ',' + ('zz' in state));
print('ownKeys =', Object.keys(state).join(','));
delete state.b;
print('after delete =', Object.keys(state).join(','));

// --- the guard every tracker uses before it decides to track ----------------
print('isFrozen(reactive) =', Object.isFrozen(state));
print('isExtensible(reactive) =', Object.isExtensible(state));

Promise.resolve().then(function () {
  print('--- after one microtask ---');
  print('log =', log.join('|'));
}).then(function () {
  print('--- after two ---');
  print('log =', log.join('|'));
});
