// Promise.race with a thenable that settles twice, and race over an empty
// array (which must never settle -- observable only as the absence of output).
var twice = { then: function (res, rej) { res('first'); rej(new Error('second')); res('third'); } };
Promise.race([twice]).then(function (v) { print('race ->', v); }, function (e) { print('race rejected', e.message); });
Promise.race([]).then(function () { print('WRONG: empty race settled'); });
Promise.resolve().then(function(){print('c1');}).then(function(){print('c2');})
  .then(function(){print('c3');});
