// A getter and a Proxy trap that run DURING a microtask flush. A reactive
// store's dependency tracking is exactly this shape.
var log = [];
var o = { get x() { log.push('get x'); return 1; } };
var p = new Proxy({ y: 2 }, { get: function (t, k) { if (typeof k === 'string') log.push('trap ' + k); return t[k]; } });
Promise.resolve().then(function () { void o.x; void p.y; print('during flush:', log.join('|')); });
void o.x; void p.y;
print('sync:', log.join('|'));
