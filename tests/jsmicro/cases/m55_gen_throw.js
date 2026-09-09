// generator.throw() and .return() ordering against the microtask queue.
async function* g() {
  try { yield 1; yield 2; } catch (e) { print('  gen caught', e.message); yield 'recovered'; }
  finally { print('  gen finally'); }
}
var it = g();
it.next().then(function (r) { print('n1', r.value); return it.throw(new Error('E')); })
        .then(function (r) { print('after throw', r.value, r.done); return it.return('R'); })
        .then(function (r) { print('after return', r.value, r.done); });
print('sync');
