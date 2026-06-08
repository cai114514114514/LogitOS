# math -- basic numeric helpers (pure AetherScript stdlib)
#   import math   /   from math import gcd, clamp, is_prime

PI = 3.141592653589793
E  = 2.718281828459045
TAU = 6.283185307179586
HALF_PI = 1.5707963267948966
EPS = 0.000000001

def abs(x):
    if x < 0:
        return -x
    return x

def sign(x):
    if x < 0:
        return -1
    if x > 0:
        return 1
    return 0

def max2(a, b):
    if a > b:
        return a
    return b

def min2(a, b):
    if a < b:
        return a
    return b

def clamp(x, lo, hi):
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

def between(x, lo, hi):
    return x >= lo and x <= hi

def sq(x):
    return x * x

def cube(x):
    return x * x * x

def powi(base, exp):                # integer exponent; negative exp returns a float
    if exp < 0:
        return 1.0 / powi(base, -exp)
    r = 1
    i = 0
    while i < exp:
        r = r * base
        i = i + 1
    return r

def pow(base, exp):                 # compatibility alias for integer exponents
    return powi(base, exp)

def gcd(a, b):
    a = abs(a)
    b = abs(b)
    while b != 0:
        t = b
        b = a % t
        a = t
    return a

def lcm(a, b):
    if a == 0 or b == 0:
        return 0
    return abs(a * b) / gcd(a, b)   # `/` is integer division for ints

def gcd_list(xs):
    if len(xs) == 0:
        return 0
    g = abs(xs[0])
    i = 1
    while i < len(xs):
        g = gcd(g, xs[i])
        i = i + 1
    return g

def lcm_list(xs):
    if len(xs) == 0:
        return 0
    m = abs(xs[0])
    i = 1
    while i < len(xs):
        m = lcm(m, xs[i])
        i = i + 1
    return m

def fact(n):
    if n < 0:
        raise "fact() needs a non-negative integer"
    r = 1
    i = 2
    while i <= n:
        r = r * i
        i = i + 1
    return r

def factorial(n):
    return fact(n)

def fib(n):
    if n < 0:
        raise "fib() needs a non-negative integer"
    a = 0
    b = 1
    i = 0
    while i < n:
        t = a + b
        a = b
        b = t
        i = i + 1
    return a

def isqrt(n):                       # integer floor of sqrt(n), n >= 0
    if n < 0:
        raise "isqrt() needs a non-negative integer"
    if n < 2:
        return n
    x = n
    y = (x + 1) / 2
    while y < x:
        x = y
        y = (x + n / x) / 2
    return x

def sqrt(x):                        # Newton sqrt; returns a float for integer input
    if x < 0:
        raise "sqrt() needs a non-negative number"
    if x == 0:
        return 0.0
    guess = x * 1.0                  # Newton converges for any x > 0 (incl. 0 < x < 1)
    i = 0
    while i < 30:
        guess = (guess + x / guess) / 2.0
        i = i + 1
    return guess

def mod_pos(x, m):
    r = x % m
    if r < 0:
        return r + abs(m)
    return r

def divmod(a, b):
    q = a / b
    return [q, a - q * b]

def is_even(n):
    return n % 2 == 0

def is_odd(n):
    return n % 2 != 0

def is_prime(n):
    if n < 2:
        return false
    if n == 2:
        return true
    if n % 2 == 0:
        return false
    p = 3
    while p * p <= n:
        if n % p == 0:
            return false
        p = p + 2
    return true

def next_prime(n):
    if n <= 2:
        return 2
    p = n
    if p % 2 == 0:
        p = p + 1
    while not is_prime(p):
        p = p + 2
    return p

def sum(xs):
    s = 0
    for x in xs:
        s = s + x
    return s

def product(xs):
    p = 1
    for x in xs:
        p = p * x
    return p

def mean(xs):
    if len(xs) == 0:
        raise "mean() needs a non-empty list"
    return sum(xs) * 1.0 / len(xs)

def close(a, b):
    return abs(a - b) <= EPS
