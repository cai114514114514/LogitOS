// A job that queues a job. Depth-first vs breadth-first is the difference
// between a batched flush converging and starving.
var n = 0;
function step() { if (++n <= 5) { print('step', n); Promise.resolve().then(step); } }
Promise.resolve().then(step);
Promise.resolve().then(function(){print('other 1');}).then(function(){print('other 2');})
  .then(function(){print('other 3');});
print('sync');
