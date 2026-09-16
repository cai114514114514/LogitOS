# aether: 3.0
# Regression program shared by the host and guest migration gates. Bit-pattern
# operations explicitly wrap; real-valued APIs convert before intermediates.
import bits
import math
import mathx
import stats
import random
import abi

def require(ok: bool, label: str) -> None:
    if not ok:
        raise ValueError(label)


minimum = -9223372036854775808
maximum = 9223372036854775807


def main() -> i64:
    require(bits.bit(63) == minimum, "sign-bit")
    require(bits.mask(63) == maximum, "mask-63")
    require(bits.rol(minimum, 1, 64) == 1, "rotate-left-sign")
    require(bits.ror(1, 1, 64) == minimum, "rotate-right-sign")
    require(bits.rol(-1, 27, 64) == -1, "rotate-all-bits")
    require(bits.rol(1, -1, 64) == minimum, "negative-rotation")
    require(bits.align_up(maximum, 1) == maximum, "already-aligned")
    require(math.isqrt(maximum) == 3037000499, "isqrt-limit")
    require(math.fib(92) == 7540113804746346429, "fib-last-representable")
    require(math.lcm(4000000000, 6000000000) == 12000000000, "lcm-reduce-first")
    # powi and median are declared `-> Any` because a negative exponent turns an
    # integer base real. A3 compares a boxed value against a boxed value rather
    # than unboxing silently, so the literal is boxed here. That is the assertion
    # these lines already made: the result IS a real, not a truncated integer.
    # Stated as "not the integer 0" rather than A2's "> 0.0". Any is Equatable
    # and not Ordered in A3, and the inequality is the sharper claim anyway: the
    # failure this guards against is 2**-64 truncating to the integer 0, which is
    # exactly what `!= Any(0)` catches and what `> 0.0` only caught in passing.
    require(math.powi(2, -64) != Any(0), "negative-power-is-real")
    require(math.powi(1, minimum) == Any(1.0), "negative-exponent-limit")
    # `maximum * 1.0` was A2's way of promoting an int to a real inside the
    # expression. A3 has no implicit promotion, so the conversion is named once
    # and reused -- the three checks below still compare against i64::MAX as a
    # real, which is the "promote FIRST, then average" property they are for.
    maximum_real = f64(maximum)
    require(math.mean([maximum, maximum]) == maximum_real, "mean-promote-first")
    require(stats.median([maximum, maximum]) == Any(maximum_real), "median-promote-first")
    require(stats.moving_average([maximum, maximum], 2)[1] == maximum_real, "average-promote-first")
    require(mathx.inverse_lerp(f64(minimum), maximum_real, 0.0) == 0.5, "wide-endpoints")
    require(mathx.hypot(4000000000.0, 0.0) > 3999999999.0, "hypot-promote-first")

    # The random generator's largest multiply fits i64, so it keeps its sequence
    # rather than being converted to floating point during migration.
    random.seed(7)
    require(random.randint(1, 10) == 7, "random-sequence")

    # The first oracle assumed every invalid syscall returned -1. FUTEX_E_ARG is
    # actually -3 on LogitOS, while the host stub returns -1. Compare the wrapper
    # with a direct call using the independently calculated packed i64 value.
    # Invalid op never waits; the high timeout bits must still marshal without an
    # arithmetic overflow before either call can reach the platform.
    word = buffer(4)
    unsafe:
        expected = syscall(SYS_FUTEX, addr(word), -4294967296, -1)
    require(expected < 0 and abi.futex(word, -1, 0, -1) == expected, "packed-sign-bit")

    # Integer APIs keep A2's checked result semantics; overflow is not a reason to
    # silently become floating point. A successful migration must catch this too.
    # A3 names the exception type rather than catching everything: `except error:`
    # bound a value and caught anything, so a DIFFERENT failure in fact() would
    # have satisfied this check.
    caught = False
    try:
        math.fact(21)
    except OverflowError as error:
        caught = True
    require(caught, "factorial-must-overflow")
    print("A2 migration ok")
    return 0
