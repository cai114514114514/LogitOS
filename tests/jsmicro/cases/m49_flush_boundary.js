// The drain boundary: everything queued during the flush must run in THIS
// flush, not the next one. A framework that schedules a re-render from inside
// a render depends on it converging before control returns.
var order = [];
function q(tag, depth) {
  Promise.resolve().then(function () {
    order.push(tag);
    if (depth > 0) q(tag + "'", depth - 1);
  });
}
q('a', 3); q('b', 3); q('c', 3);
Promise.resolve().then(function(){}).then(function(){}).then(function(){}).then(function(){})
  .then(function () { print(order.join(' ')); });
print('sync');
