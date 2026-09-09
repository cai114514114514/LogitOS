// The floor: does a microtask run after the whole synchronous block, and do
// promise reactions and queueMicrotask share one FIFO queue?
print('A sync-start');
Promise.resolve().then(function () { print('C then-1'); });
queueMicrotask(function () { print('D qmt-1'); });
Promise.resolve().then(function () { print('E then-2'); });
queueMicrotask(function () { print('F qmt-2'); });
print('B sync-end');
