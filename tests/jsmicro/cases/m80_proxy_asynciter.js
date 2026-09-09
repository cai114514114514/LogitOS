// Symbol.asyncIterator reached through a Proxy get trap.
var target = { async *[Symbol.asyncIterator]() { yield 1; yield 2; } };
var keys = [];
var px = new Proxy(target, { get: function (t, k, r) { keys.push(String(k)); return Reflect.get(t, k, r); } });
(async function () { for await (const v of px) print('v', v); print('keys', keys.join(',')); })();
print('sync');
