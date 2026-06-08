# mathx -- higher-level numeric helpers and float approximations
#   import mathx   /   from mathx import sin, cos, lerp, dist

PI = 3.141592653589793
E = 2.718281828459045
TAU = 6.283185307179586
HALF_PI = 1.5707963267948966
DEG = 0.017453292519943295
RAD = 57.29577951308232

def _abs(x):
    if x < 0:
        return -x
    return x

def square(x):
    return x * x

def cube(x):
    return x * x * x

def quad(x):
    return square(square(x))   # resolves module-mate `square`

def clamp(x, lo, hi):
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x

def lerp(a, b, t):
    return a + (b - a) * t

def inverse_lerp(a, b, x):
    if a == b:
        raise "inverse_lerp() needs distinct endpoints"
    return (x - a) * 1.0 / (b - a)      # *1.0: a normalized fraction, not int division

def remap(in_lo, in_hi, out_lo, out_hi, x):
    return lerp(out_lo, out_hi, inverse_lerp(in_lo, in_hi, x))

def smoothstep(edge0, edge1, x):
    t = clamp(inverse_lerp(edge0, edge1, x), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)

def sqrt(x):
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

def hypot(x, y):
    return sqrt(x * x + y * y)

def dist2(ax, ay, bx, by):
    dx = ax - bx
    dy = ay - by
    return dx * dx + dy * dy

def dist(ax, ay, bx, by):
    return sqrt(dist2(ax, ay, bx, by))

def deg_to_rad(deg):
    return deg * DEG

def rad_to_deg(rad):
    return rad * RAD

def _wrap_pi(x):
    x = x * 1.0
    while x > PI:
        x = x - TAU
    while x < -PI:
        x = x + TAU
    return x

def sin(x):                         # Taylor approximation, good for small UI math
    x = _wrap_pi(x)
    term = x
    out = x
    i = 1
    while i < 8:
        term = -term * x * x / ((2 * i) * (2 * i + 1))
        out = out + term
        i = i + 1
    return out

def cos(x):
    x = _wrap_pi(x)
    term = 1.0
    out = 1.0
    i = 1
    while i < 8:
        term = -term * x * x / ((2 * i - 1) * (2 * i))
        out = out + term
        i = i + 1
    return out

def tan(x):
    c = cos(x)
    if _abs(c) < 0.000000001:
        raise "tan() undefined near odd pi/2"
    return sin(x) / c

def exp(x):
    if x < 0:
        return 1.0 / exp(-x)
    halves = 0
    y = x * 1.0
    while y > 1.0:
        y = y / 2.0
        halves = halves + 1
    term = 1.0
    out = 1.0
    i = 1
    while i < 22:
        term = term * y / i
        out = out + term
        i = i + 1
    while halves > 0:
        out = out * out
        halves = halves - 1
    return out

def ln(x):                          # natural log via atanh transform
    if x <= 0:
        raise "ln() needs a positive number"
    # Argument reduction: ln(x) = k + ln(x / e^k). Pull x near 1 first, else the
    # atanh series (ratio y^2, y->1 for large x) converges far too slowly.
    x = x * 1.0
    k = 0
    while x > 1.5:
        x = x / E
        k = k + 1
    while x < 0.6:
        x = x * E
        k = k - 1
    y = (x - 1.0) / (x + 1.0)
    y2 = y * y
    term = y
    out = 0.0
    n = 1
    i = 0
    while i < 30:
        out = out + term / n
        term = term * y2
        n = n + 2
        i = i + 1
    return k + 2.0 * out

def powf(base, power):
    if base <= 0:
        raise "powf() needs a positive base"
    return exp(ln(base) * power)
