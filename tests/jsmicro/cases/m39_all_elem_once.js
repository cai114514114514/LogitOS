// Promise.all's per-element resolve function must fire at most once, and the
// iterator must be read exactly once per element.
var reads = 0;
var iterable = { [Symbol.iterator]() { var i = 0; return { next() { reads++; return i < 3 ? {value: i++, done:false} : {done:true}; } }; } };
Promise.all(iterable).then(function (a) { print('all', a.join(','), 'next calls', reads); });
var evil = { then: function (res) { res(1); res(2); res(3); } };
Promise.all([evil, evil]).then(function (a) { print('evil', a.join(',')); });
