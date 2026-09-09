// A job queued by a job queued by a job: three levels, interleaved with a flat
// chain, so the queue discipline (FIFO, not a stack) is visible.
var out = [];
Promise.resolve().then(function () {
  out.push('L1');
  Promise.resolve().then(function () {
    out.push('L2');
    Promise.resolve().then(function () { out.push('L3'); });
  });
});
Promise.resolve().then(function () { out.push('F1'); })
  .then(function () { out.push('F2'); })
  .then(function () { out.push('F3'); })
  .then(function () { out.push('F4'); })
  .then(function () { print(out.join(' ')); });
print('sync');
