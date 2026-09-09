// A Proxy over a promise: `then` comes through the get trap, and the engine
// must not shortcut to the target's internal slots.
var real = Promise.resolve('V');
var traps = [];
var px = new Proxy(real, { get: function (t, k, r) { if (typeof k === 'string') traps.push(k); var v = t[k]; return typeof v === 'function' ? v.bind(t) : v; } });
Promise.resolve(px).then(function (v) { print('resolved', v, '| traps', traps.join(',')); });
print('is promise?', px instanceof Promise);
