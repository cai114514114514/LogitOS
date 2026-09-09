// await must NOT call valueOf/Symbol.toPrimitive on its operand; it only reads
// `then`. An engine that coerces runs a framework's lazy getter at the wrong time.
var o = {
  valueOf: function () { print('  valueOf'); return 1; },
  toString: function () { print('  toString'); return 's'; },
  get then() { print('  then read'); return undefined; }
};
(async function () { var v = await o; print('awaited, same object?', v === o); })();
print('sync');
