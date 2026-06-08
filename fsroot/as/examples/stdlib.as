# stdlib demo: import the LibAether modules and exercise them.
import seq
import math
import dicts

print("squares:", seq.map(lambda x: x * x, [1, 2, 3, 4]))
print("evens:", seq.filter(lambda x: x % 2 == 0, range(10)))
print("sum 1..10:", seq.sum(range(1, 11)))
print("sorted:", seq.sorted([3, 1, 4, 1, 5, 9, 2, 6]))
print("gcd/lcm:", math.gcd(24, 36), math.lcm(4, 6))
print("isqrt 144:", math.isqrt(144))
print("merged:", dicts.merge({"a": 1}, {"b": 2}))
print("stdlib ok")
