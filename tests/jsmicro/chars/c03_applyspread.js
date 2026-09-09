// The single most common place a page meets the stack ceiling: the
// String.fromCharCode.apply / Math.max.apply chunking idiom that every
// binary-to-string helper in every bundle contains.
var big = [];
for (var i = 0; i < 200000; i++) big.push(65);
try { var s = String.fromCharCode.apply(null, big); print('fromCharCode.apply len', s.length); }
catch (e) { print('fromCharCode.apply threw', e.constructor.name, JSON.stringify(e.message)); }
try { print('Math.max.apply', Math.max.apply(null, big)); }
catch (e) { print('Math.max.apply threw', e.constructor.name, JSON.stringify(e.message)); }
try { var t = [].concat(...[big]); print('spread len', t.length); }
catch (e) { print('spread threw', e.constructor.name, JSON.stringify(e.message)); }
