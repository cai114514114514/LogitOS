// Promise.all over 2000 already-resolved promises: the settle order of the
// per-element reactions relative to a plain chain.
var arr = [];
for (var i = 0; i < 2000; i++) arr.push(Promise.resolve(i));
var ticks = 0;
function tick() { ticks++; if (ticks < 12) Promise.resolve().then(tick); }
Promise.resolve().then(tick);
Promise.all(arr).then(function (a) { print('all len', a.length, 'first', a[0], 'last', a[1999], 'ticks elapsed', ticks); });
print('sync');
