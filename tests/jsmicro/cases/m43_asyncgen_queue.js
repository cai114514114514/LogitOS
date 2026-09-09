// An async generator's request QUEUE: two next() calls before the first
// resolves must be served in order, and the second must not start the body
// until the first yield has been delivered.
async function* g() { print('  body 1'); yield 'A'; print('  body 2'); yield 'B'; print('  body 3'); }
var it = g();
it.next().then(function (r) { print('n1', r.value, r.done); });
it.next().then(function (r) { print('n2', r.value, r.done); });
it.next().then(function (r) { print('n3', r.value, r.done); });
it.next().then(function (r) { print('n4', r.value, r.done); });
print('sync');
