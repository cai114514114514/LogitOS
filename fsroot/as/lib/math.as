# aether: 3.0
# Basic numeric helpers. Generic arithmetic keeps the concrete native width;
# integer overflow remains observable in both debug and optimized programs.
PI: f64 = 3.141592653589793
E: f64 = 2.718281828459045
TAU: f64 = 6.283185307179586
HALF_PI: f64 = 1.5707963267948966
EPS: f64 = 0.000000001


def abs[T: Number](x: T) -> T:
    return -x if x < T(0) else x


def sign[T: Number](x: T) -> i64:
    if x < T(0):
        return -1
    if x > T(0):
        return 1
    return 0


def max2[T: Number](a: T, b: T) -> T:
    return a if a > b else b


def min2[T: Number](a: T, b: T) -> T:
    return a if a < b else b


def clamp[T: Number](x: T, lo: T, hi: T) -> T:
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x


def between[T: Number](x: T, lo: T, hi: T) -> bool:
    return x >= lo and x <= hi


def sq[T: Number](x: T) -> T:
    return x * x


def cube[T: Number](x: T) -> T:
    return x * x * x


def _positive_power[T: Number](base: T, exponent: i64) -> T:
    result = T(1)
    factor = base
    remaining = exponent
    while remaining != 0:
        if remaining % 2 != 0:
            result *= factor
        remaining /= 2
        if remaining != 0:
            factor *= factor
    return result


def _negative_power[T: Number](base: T, exponent: i64) -> f64:
    # Keep the exponent negative: its positive magnitude need not fit i64.
    # Convert before the first divide/multiply, as in the corrected old rule.
    result = 1.0
    factor = 1.0 / f64(base)
    remaining = exponent
    while remaining != 0:
        if remaining % 2 != 0:
            result *= factor
        remaining /= 2
        if remaining != 0:
            factor *= factor
    return result


def powi[T: Number](base: T, exp: i64) -> Any:
    # This API historically returned a value-dependent type. Any makes that
    # boundary explicit without rounding positive integer powers to f64.
    # Only the returned value is boxed; the loops use native T/f64 arithmetic.
    if exp == 0:
        return Any(1)
    if exp < 0:
        return Any(_negative_power(base, exp))
    return Any(_positive_power(base, exp))


def pow[T: Number](base: T, exp: i64) -> Any:
    return powi(base, exp)


def gcd[T: Number](a: T, b: T) -> T:
    left = abs(a)
    right = abs(b)
    while right != T(0):
        previous = right
        right = left % right
        left = previous
    return left


def lcm[T: Number](a: T, b: T) -> T:
    if a == T(0) or b == T(0):
        return T(0)
    # Dividing the common factor first avoids an overflowing intermediate when
    # the actual least common multiple still fits the caller's integer width.
    return abs((a / gcd(a, b)) * b)


def gcd_list[T: Number](xs: List[T]) -> T:
    result = T(0)
    for item in xs:
        result = gcd(result, item)
    return result


def lcm_list[T: Number](xs: List[T]) -> T:
    if len(xs) == 0:
        return T(0)
    result = abs(xs[0])
    for index in range(1, len(xs)):
        result = lcm(result, xs[index])
    return result


def fact(n: i64) -> i64:
    if n < 0:
        raise ValueError("fact() needs a non-negative integer")
    result = 1
    factor = 2
    while factor <= n:
        result *= factor
        factor += 1
    return result


def factorial(n: i64) -> i64:
    return fact(n)


def fib(n: i64) -> i64:
    if n < 0:
        raise ValueError("fib() needs a non-negative integer")
    if n == 0:
        return 0
    previous = 0
    current = 1
    for index in range(1, n):
        following = previous + current
        previous = current
        current = following
    # Do not compute F(n+1) to return F(n): F(92) fits, F(93) does not.
    return current


def isqrt(n: i64) -> i64:
    if n < 0:
        raise ValueError("isqrt() needs a non-negative integer")
    if n < 2:
        return n
    estimate = n
    # ceil(n / 2) avoids n + 1 overflowing at the signed maximum.
    improved = estimate / 2 + estimate % 2
    while improved < estimate:
        estimate = improved
        improved = (estimate + n / estimate) / 2
    return estimate


def sqrt[T: Number](x: T) -> f64:
    real = f64(x)
    if real < 0.0:
        raise ValueError("sqrt() needs a non-negative number")
    if real == 0.0:
        return 0.0
    # Preserve the original bounded approximation; this is not a libm accuracy
    # promise over the entire floating-point exponent range.
    guess = real
    for iteration in range(30):
        guess = (guess + real / guess) / 2.0
    return guess


def mod_pos[T: Number](x: T, m: T) -> T:
    remainder = x % m
    return remainder + abs(m) if remainder < T(0) else remainder


def divmod[T: Number](a: T, b: T) -> List[T]:
    quotient = a / b
    return [quotient, a - quotient * b]


def is_even(n: i64) -> bool:
    return n % 2 == 0


def is_odd(n: i64) -> bool:
    return n % 2 != 0


def is_prime(n: i64) -> bool:
    if n < 2:
        return false
    if n == 2:
        return true
    if n % 2 == 0:
        return false
    divisor = 3
    while divisor <= n / divisor:
        if n % divisor == 0:
            return false
        divisor += 2
    return true


def next_prime(n: i64) -> i64:
    if n <= 2:
        return 2
    candidate = n
    if candidate % 2 == 0:
        candidate += 1
    while not is_prime(candidate):
        candidate += 2
    return candidate


def sum[T: Number](xs: List[T]) -> T:
    total = T(0)
    for item in xs:
        total += item
    return total


def product[T: Number](xs: List[T]) -> T:
    result = T(1)
    for item in xs:
        result *= item
    return result


def mean[T: Number](xs: List[T]) -> f64:
    if len(xs) == 0:
        raise ValueError("mean() needs a non-empty list")
    # Accumulate real values from the first element. Summing i64 first can
    # overflow even when the mean of those same inputs is representable.
    total = 0.0
    for item in xs:
        total += f64(item)
    return total / f64(len(xs))


def close[T: Number](a: T, b: T) -> bool:
    return f64(abs(a - b)) <= EPS
