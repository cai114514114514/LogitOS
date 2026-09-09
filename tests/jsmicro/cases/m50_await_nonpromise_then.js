// awaiting a value whose CONSTRUCTOR is a promise subclass, and awaiting a
// value with a then that is not callable (must resolve immediately).
var notThenable = { then: 42 };
(async function () { var v = await notThenable; print('non-callable then ->', v.then); })();
class P extends Promise {}
(async function () { var v = await P.resolve('S'); print('subclass await ->', v); })();
Promise.resolve().then(function(){print('h1');}).then(function(){print('h2');})
  .then(function(){print('h3');}).then(function(){print('h4');});
print('sync');
