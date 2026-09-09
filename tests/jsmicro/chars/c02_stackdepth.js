// CHARACTERIZATION, not a diff: the recursion ceiling is a property of the
// build, so node and this engine are expected to differ in the NUMBER. What
// matters is the exception's CONSTRUCTOR and message, because that is what a
// framework's chunking guard matches on.
function d(n) { return n <= 0 ? 0 : d(n - 1) + 1; }
var lo = 0, hi = 4000000;
while (lo < hi) { var mid = (lo + hi + 1) >> 1; try { d(mid); lo = mid; } catch (e) { hi = mid - 1; } }
print('max plain recursion depth ~', lo);
try { d(lo + 5000); } catch (e) {
  print('ctor', e.constructor.name, '| name', e.name, '| message', JSON.stringify(e.message));
  print('instanceof RangeError', e instanceof RangeError, '| instanceof Error', e instanceof Error);
}
