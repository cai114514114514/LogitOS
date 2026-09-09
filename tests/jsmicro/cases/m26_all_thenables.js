// Promise.all over thenables: each costs the thenable job, and the result array
// must be in INPUT order however they settle.
function th(tag, ticks) {
  return { then: function (res) {
    var p = Promise.resolve();
    for (var i = 0; i < ticks; i++) p = p.then(function(){});
    p.then(function () { print('  settle ' + tag); res(tag); });
  } };
}
Promise.all([th('a', 5), th('b', 1), th('c', 3)]).then(function (a) { print('all ->', a.join(',')); });
Promise.resolve().then(function(){print('q1');}).then(function(){print('q2');})
  .then(function(){print('q3');}).then(function(){print('q4');})
  .then(function(){print('q5');}).then(function(){print('q6');})
  .then(function(){print('q7');}).then(function(){print('q8');});
print('sync');
