# stdlib demo: import the LibLogit modules and exercise them.
import seq
import math
import dicts
import strings
import sets
import stats
import random
import paths
import bits
import mathx
import test

print("squares:", seq.map(lambda x: x * x, [1, 2, 3, 4]))
print("evens:", seq.filter(lambda x: x % 2 == 0, range(10)))
print("sum 1..10:", seq.sum(range(1, 11)))
print("sorted:", seq.sorted([3, 1, 4, 1, 5, 9, 2, 6]))
print("gcd/lcm:", math.gcd(24, 36), math.lcm(4, 6))
print("isqrt 144:", math.isqrt(144))
print("merged:", dicts.merge({"a": 1}, {"b": 2}))
print("chunks:", seq.chunk(range(1, 6), 2))
print("prime:", math.next_prime(100))
print("words:", strings.split("logit script stdlib", " "))
print("set union:", sets.to_list(sets.union(sets.from_list([1, 2]), sets.from_list([2, 3]))))
print("stats:", stats.mean([1, 2, 3, 4]), stats.median([5, 1, 2]))
random.seed(7)
print("random:", random.randint(1, 10), random.choice(["a", "b", "c"]))
print("path:", paths.dirname("/usr/as/lib/math.as"), paths.basename("/usr/as/lib/math.as"), paths.extname("math.as"))
print("bits:", bits.has(10, 2), bits.count_ones(11), bits.align_up(17, 8))

# --- regression self-test: guards the fixed stdlib bugs. test-as-os greps for
#     "stdlib ok", which now prints ONLY if every assertion below passes. ---
test.assert_eq(math.sqrt(0.25), 0.5, "math.sqrt fraction")
test.assert(math.abs(mathx.ln(1000) - 6.907755278982137) < 0.001, "mathx.ln large arg")
test.assert(math.abs(mathx.powf(100, 2) - 10000.0) < 0.001, "mathx.powf via ln")
test.assert_eq(mathx.inverse_lerp(0, 10, 5), 0.5, "mathx.inverse_lerp int args")
test.assert_eq(bits.mask(64), -1, "bits.mask(64)")
test.assert_eq(bits.rol(1, 1, 64), 2, "bits.rol width 64")
test.assert_eq(paths.extname("..foo"), "", "paths.extname leading dots")
test.assert_eq(paths.stem("..foo"), "..foo", "paths.stem leading dots")
test.assert_same(seq.drop([1, 2, 3], -1), [1, 2, 3], "seq.drop negative n")
if test.failed() == 0:
    print("stdlib ok")
else:
    print("stdlib SELFTEST FAILED:", test.failed())
