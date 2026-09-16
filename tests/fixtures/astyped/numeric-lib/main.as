# aether: 3.0
import mathx
import math
import random
from mathx import PI


def near(actual: f64, expected: f64, tolerance: f64) -> None:
    difference = actual - expected
    assert difference < tolerance and difference > -tolerance


def math_checks() -> None:
    assert PI == 3.141592653589793
    assert mathx.TAU == 2.0 * PI
    assert mathx.HALF_PI == PI / 2.0
    assert mathx.E > 2.718 and mathx.E < 2.719
    assert mathx.DEG > 0.017 and mathx.RAD > 57.29
    assert mathx.square(i8(5)) == i8(25)
    assert mathx.cube(4) == 64
    assert mathx.quad(2.0) == 16.0
    assert mathx.clamp(9, 2, 7) == 7
    assert mathx.clamp(-1.0, 0.0, 1.0) == 0.0
    near(mathx.lerp(10.0, 20.0, 0.25), 12.5, 0.000001)
    near(mathx.inverse_lerp(-9223372036854775808.0, 9223372036854775807.0, 0.0), 0.5, 0.000001)
    near(mathx.remap(0.0, 100.0, 10.0, 20.0, 25.0), 12.5, 0.000001)
    near(mathx.smoothstep(0.0, 1.0, 0.5), 0.5, 0.000001)
    near(mathx.sqrt(9.0), 3.0, 0.000001)
    assert mathx.sqrt(0.0) == 0.0
    near(mathx.hypot(3.0, 4.0), 5.0, 0.000001)
    assert mathx.dist2(0, 0, 3, 4) == 25
    near(mathx.dist(0.0, 0.0, 3.0, 4.0), 5.0, 0.000001)
    near(mathx.deg_to_rad(180.0), PI, 0.000001)
    near(mathx.rad_to_deg(PI), 180.0, 0.000001)
    near(mathx.sin(mathx.HALF_PI), 1.0, 0.000001)
    near(mathx.sin(-mathx.TAU), 0.0, 0.000001)
    near(mathx.cos(PI), -1.0, 0.00001)
    near(mathx.tan(PI / 4.0), 1.0, 0.000001)
    near(mathx.exp(1.0), mathx.E, 0.000001)
    near(mathx.exp(-1.0), 1.0 / mathx.E, 0.000001)
    near(mathx.exp(2.0), mathx.E * mathx.E, 0.000001)
    near(mathx.ln(mathx.E), 1.0, 0.000001)
    near(mathx.ln(0.1), -2.302585092994046, 0.000001)
    near(mathx.powf(2.0, 3.0), 8.0, 0.000001)
    caught = 0
    try:
        mathx.inverse_lerp(2.0, 2.0, 1.0)
    except ValueError:
        caught += 1
    try:
        mathx.sqrt(-1.0)
    except ValueError:
        caught += 1
    try:
        mathx.tan(mathx.HALF_PI)
    except ValueError:
        caught += 1
    try:
        mathx.ln(0.0)
    except ValueError:
        caught += 1
    try:
        mathx.powf(0.0, 2.0)
    except ValueError:
        caught += 1
    try:
        mathx.square(i8(12))
    except OverflowError:
        caught += 1
    assert caught == 6


def random_checks() -> None:
    assert random.seed(0) == 1
    assert random.seed(-1) == 2147483647
    assert random.seed(-9223372036854775808) == 1
    assert random.seed(7) == 7
    near(random.random(), 0.5970560554414988, 0.000000000000001)
    random.seed(7)
    assert random.randint(1, 10) == 7
    random.seed(7)
    assert random.randrange(0, 10) == 6
    random.seed(7)
    assert not random.bool()
    assert random.bool()
    values = [1, 2, 3, 4]
    random.seed(7)
    shuffled = random.shuffle(values)
    assert values[0] == 1 and values[3] == 4
    assert shuffled[0] == 2 and shuffled[1] == 3
    assert shuffled[2] == 4 and shuffled[3] == 1
    random.seed(7)
    selected = random.sample(values, 2)
    assert len(selected) == 2 and selected[0] == 2 and selected[1] == 3
    random.seed(7)
    words = ["native " + "choice", "other"]
    word = random.choice(words)
    gc_collect()
    assert word == "native choice"
    assert len(random.sample(words, 0)) == 0
    empty: List[str] = []
    caught = 0
    try:
        random.choice(empty)
    except ValueError:
        caught += 1
    try:
        random.randint(2, 1)
    except ValueError:
        caught += 1
    try:
        random.randrange(2, 2)
    except ValueError:
        caught += 1
    try:
        random.sample(values, 5)
    except ValueError:
        caught += 1
    try:
        random.sample(values, -1)
    except ValueError:
        caught += 1
    try:
        random.randint(-9223372036854775808, 9223372036854775807)
    except OverflowError:
        caught += 1
    assert caught == 6


def basic_math_checks() -> None:
    assert math.PI == PI and math.TAU == mathx.TAU
    assert math.E == mathx.E and math.HALF_PI == mathx.HALF_PI
    assert math.EPS == 0.000000001
    assert math.abs(-7) == 7
    assert math.abs(u64(7)) == u64(7)
    assert math.sign(-1.0) == -1 and math.sign(0) == 0
    assert math.sign(2) == 1
    assert math.max2(2, 7) == 7 and math.min2(2.0, 7.0) == 2.0
    assert math.clamp(8, 2, 5) == 5 and math.between(3, 2, 5)
    assert math.sq(3) == 9 and math.cube(3.0) == 27.0
    assert cast[i64](math.powi(2, 62)) == 4611686018427387904
    assert cast[f64](math.powi(2, -64)) > 0.0
    assert cast[f64](math.powi(1, -9223372036854775808)) == 1.0
    assert cast[i64](math.powi(2.5, 0)) == 1
    assert cast[f64](math.pow(2.0, 3)) == 8.0
    assert math.gcd(48, 18) == 6
    assert math.lcm(4000000000, 6000000000) == 12000000000
    assert math.gcd_list([24, 18, 30]) == 6
    assert math.lcm_list([3, 4, 5]) == 60
    empty: List[i64] = []
    assert math.gcd_list(empty) == 0 and math.lcm_list(empty) == 0
    assert math.fact(20) == 2432902008176640000
    assert math.factorial(0) == 1
    assert math.fib(0) == 0 and math.fib(92) == 7540113804746346429
    assert math.isqrt(9223372036854775807) == 3037000499
    near(math.sqrt(4), 2.0, 0.000001)
    assert math.mod_pos(-7, 3) == 2
    pair = math.divmod(-7, 3)
    assert pair[0] == -2 and pair[1] == -1
    assert math.is_even(-4) and math.is_odd(-3)
    assert math.is_prime(97) and not math.is_prime(99)
    assert math.next_prime(98) == 101
    assert math.sum([1, 2, 3]) == 6 and math.sum(empty) == 0
    assert math.product([2.0, 3.0]) == 6.0 and math.product(empty) == 1
    near(math.mean([1, 2, 3, 4]), 2.5, 0.000001)
    assert math.mean([9223372036854775807, 9223372036854775807]) == 9223372036854775807.0
    assert math.close(1.0, 1.0000000001)
    caught = 0
    try:
        math.abs(-9223372036854775808)
    except OverflowError:
        caught += 1
    try:
        math.fact(21)
    except OverflowError:
        caught += 1
    try:
        math.powi(2, 63)
    except OverflowError:
        caught += 1
    try:
        math.powi(0, -1)
    except ZeroDivisionError:
        caught += 1
    try:
        math.fact(-1)
    except ValueError:
        caught += 1
    try:
        math.fib(-1)
    except ValueError:
        caught += 1
    try:
        math.isqrt(-1)
    except ValueError:
        caught += 1
    try:
        math.sqrt(-1)
    except ValueError:
        caught += 1
    try:
        math.mean(empty)
    except ValueError:
        caught += 1
    assert caught == 9


def main() -> None:
    math_checks()
    basic_math_checks()
    random_checks()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native numeric library ok")
