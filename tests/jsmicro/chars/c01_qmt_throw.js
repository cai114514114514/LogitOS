// A throwing microtask must not stop the ones queued after it, and it must not
// be swallowed into a rejection nobody can see. queueMicrotask here is
// Promise.resolve().then(fn), so a throw inside it becomes a REJECTION of an
// internal promise with no handler -- silent -- where a real queueMicrotask
// reports the error. Print what survives.
var survived = [];
queueMicrotask(function () { survived.push('a'); throw new Error('boom'); });
queueMicrotask(function () { survived.push('b'); });
Promise.resolve().then(function () { survived.push('c'); });
Promise.resolve().then(function () { print('survivors:', survived.join(',')); });
Promise.resolve().then(function(){}).then(function () { print('final:', survived.join(',')); });
