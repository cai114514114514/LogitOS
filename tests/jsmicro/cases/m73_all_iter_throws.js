// Promise.all over an iterable that throws mid-iteration must reject, and must
// close the iterator.
var it = { [Symbol.iterator]() { var i = 0; return { next() { if (i++ === 2) throw new Error('iter'); return {value: i, done: false}; }, return() { print('  iterator closed'); return {done:true}; } }; } };
Promise.all(it).then(function () { print('WRONG resolved'); }, function (e) { print('rejected', e.message); });
var it2 = { [Symbol.iterator]() { var i = 0; return { next() { return i++ < 2 ? {value: Promise.reject(new Error('el')), done:false} : {done:true}; }, return() { print('  closed2'); return {done:true}; } }; } };
Promise.all(it2).catch(function (e) { print('rejected2', e.message); });
