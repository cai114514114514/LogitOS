# aether: 3.0
# Higher-level numeric helpers. Real-valued operations accept f64 explicitly;
# shape-preserving arithmetic specializes for each declared Number type.
PI: f64 = 3.141592653589793
E: f64 = 2.718281828459045
TAU: f64 = 6.283185307179586
HALF_PI: f64 = 1.5707963267948966
DEG: f64 = 0.017453292519943295
RAD: f64 = 57.29577951308232


def _abs(x: f64) -> f64:
    return -x if x < 0.0 else x


def square[T: Number](x: T) -> T:
    return x * x


def cube[T: Number](x: T) -> T:
    return x * x * x


def quad[T: Number](x: T) -> T:
    return square(square(x))


def clamp[T: Number](x: T, lo: T, hi: T) -> T:
    if x < lo:
        return lo
    if x > hi:
        return hi
    return x


def lerp(a: f64, b: f64, t: f64) -> f64:
    # The old dynamic implementation promoted endpoints internally. The native
    # boundary is f64, so no signed integer subtraction can overflow first.
    return a + (b - a) * t


def inverse_lerp(a: f64, b: f64, x: f64) -> f64:
    if a == b:
        raise ValueError("inverse_lerp() needs distinct endpoints")
    return (x - a) / (b - a)


def remap(in_lo: f64, in_hi: f64, out_lo: f64, out_hi: f64, x: f64) -> f64:
    return lerp(out_lo, out_hi, inverse_lerp(in_lo, in_hi, x))


def smoothstep(edge0: f64, edge1: f64, x: f64) -> f64:
    t = clamp(inverse_lerp(edge0, edge1, x), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def sqrt(x: f64) -> f64:
    if x < 0.0:
        raise ValueError("sqrt() needs a non-negative number")
    if x == 0.0:
        return 0.0
    # Preserve the original 30 Newton steps. This is an approximation helper,
    # not a correctly rounded libm implementation for every f64 magnitude.
    guess = x
    for iteration in range(30):
        guess = (guess + x / guess) / 2.0
    return guess


def hypot(x: f64, y: f64) -> f64:
    return sqrt(x * x + y * y)


def dist2[T: Number](ax: T, ay: T, bx: T, by: T) -> T:
    dx = ax - bx
    dy = ay - by
    return dx * dx + dy * dy


def dist(ax: f64, ay: f64, bx: f64, by: f64) -> f64:
    return sqrt(dist2(ax, ay, bx, by))


def deg_to_rad(deg: f64) -> f64:
    return deg * DEG


def rad_to_deg(rad: f64) -> f64:
    return rad * RAD


def _wrap_pi(x: f64) -> f64:
    reduced = x
    while reduced > PI:
        reduced -= TAU
    while reduced < -PI:
        reduced += TAU
    return reduced


def sin(x: f64) -> f64:
    # Range reduction and eight terms preserve the existing small-UI-math
    # approximation. Integer series counters are converted before division.
    reduced = _wrap_pi(x)
    term = reduced
    result = reduced
    for index in range(1, 8):
        denominator = f64((2 * index) * (2 * index + 1))
        term = -term * reduced * reduced / denominator
        result += term
    return result


def cos(x: f64) -> f64:
    reduced = _wrap_pi(x)
    term = 1.0
    result = 1.0
    for index in range(1, 8):
        denominator = f64((2 * index - 1) * (2 * index))
        term = -term * reduced * reduced / denominator
        result += term
    return result


def tan(x: f64) -> f64:
    cosine = cos(x)
    if _abs(cosine) < 0.000000001:
        raise ValueError("tan() undefined near odd pi/2")
    return sin(x) / cosine


def exp(x: f64) -> f64:
    if x < 0.0:
        return 1.0 / exp(-x)
    halves = 0
    reduced = x
    while reduced > 1.0:
        reduced /= 2.0
        halves += 1
    term = 1.0
    result = 1.0
    for index in range(1, 22):
        term = term * reduced / f64(index)
        result += term
    while halves > 0:
        result *= result
        halves -= 1
    return result


def ln(x: f64) -> f64:
    if x <= 0.0:
        raise ValueError("ln() needs a positive number")
    # ln(x) = scale + ln(x / e^scale). Without reduction the atanh series
    # approaches ratio 1 on large inputs and 30 terms are insufficient.
    reduced = x
    scale = 0
    while reduced > 1.5:
        reduced /= E
        scale += 1
    while reduced < 0.6:
        reduced *= E
        scale -= 1
    ratio = (reduced - 1.0) / (reduced + 1.0)
    squared = ratio * ratio
    term = ratio
    result = 0.0
    denominator = 1
    for iteration in range(30):
        result += term / f64(denominator)
        term *= squared
        denominator += 2
    return f64(scale) + 2.0 * result


def powf(base: f64, power: f64) -> f64:
    if base <= 0.0:
        raise ValueError("powf() needs a positive base")
    return exp(ln(base) * power)
