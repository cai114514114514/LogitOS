// A throwing microtask must not stop the ones queued after it.
queueMicrotask(function () { print('m-a'); });
Promise.resolve().then(function () { print('m-b'); });
Promise.resolve().then(function () { print('m-c'); });
print('sync');
