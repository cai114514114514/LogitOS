// Deep SYNCHRONOUS recursion inside a microtask. The guest is built with
// -DCONFIG_STACK_CHECK and a 2 MiB limit; most host gates in this tree are not.
// The engine must throw, not die, and the throw must be catchable.
function deep(n) { return n <= 0 ? 0 : deep(n - 1) + 1; }
Promise.resolve().then(function () {
  var caught = null, got = -1;
  try { got = deep(200000); } catch (e) { caught = e.name; }
  print('deep result', got, 'caught', caught);
});
Promise.resolve().then(function () {}).then(function () { print('queue survived'); });
