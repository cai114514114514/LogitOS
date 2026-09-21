# aether: 3.0
# A4-F3 edge rules for **= &= |= ^= <<= >>=. The binary machinery these route
# onto (checked_power, at_ir_shift) predates the compound forms; this fixture
# exists to prove the ROUTING inherits the checks, because a compound lowered
# as an unchecked raw op would print the same successes and only differ on
# exactly these edges. Keep the two values labeled below stable: the
# as_assignment_test jar control replays this fixture against a mutated
# as_compound_operator, and an output DIFF is what proves the second door
# (cli/eval.c) cannot drift from the jar (frontend/lexer.h) without a gate
# going red on both engines at once.
def main() -> None:
    n: i64 = 12
    n &= 10
    n |= 1
    n ^= 5
    assert n == 12

    s: u8 = 100
    s <<= 1
    assert s == 200
    s >>= 2
    assert s == 50

    # Sign extension: the arithmetic right shift keeps the top bit, so the
    # widened print shows a large positive u64 bit pattern, never 7.
    neg: i8 = -16
    neg >>= 1
    assert neg == -8

    p: i8 = 2
    p **= 3
    assert p == 8
    z: i64 = 0
    z **= 0
    assert z == 1

    # 100*100 would overflow i8, but that square is the FINAL unnecessary one:
    # exponentiation by squaring must exit before computing it. Any "optimize
    # the loop" change that squares unconditionally turns this green case red.
    hundred: i8 = 100
    hundred **= 1
    assert hundred == 100

    # <<= must round-trip: shifting 64 out of a u8 loses it.
    u: u8 = 64
    try:
        u <<= 2
        assert False
    except OverflowError:
        pass
    assert u == 64

    # The count test is UNSIGNED ((uint64_t)e >= width), so a negative count
    # is huge-as-unsigned and refused by the same compare -- a signed >= 0
    # precheck would be a second door for the same rule.
    v: i64 = 1
    try:
        v <<= -1
        assert False
    except ValueError:
        pass
    try:
        v <<= 64
        assert False
    except ValueError:
        pass
    # The round trip checks the RESULT is representable (LANGUAGE_3.md
    # section 3), not that no bit moved: -1<<1 is -2 and ashr(-2,1) restores
    # -1 exactly, so sign replication raises nothing -- matching the binary
    # `<<`, which also prints -2. The committed form expected OverflowError
    # here and made this gate red from birth; 1<<63 below remains the
    # sign-change OverflowError case.
    m: i64 = -1
    m <<= 1
    assert m == -2
    top: i64 = 1
    try:
        top <<= 63
        assert False
    except OverflowError:
        pass
    assert top == 1
    uv: u64 = 1
    uv <<= 63
    assert uv == 9223372036854775808
    uv >>= 63
    assert uv == 1

    try:
        p **= -1
        assert False
    except ValueError:
        pass

    # Result type is the TARGET's type: u8 **= squares in u8, so 16*16 == 256
    # overflows the same way the plain binary form does -- the compound form
    # must not silently widen to i64 and store a truncated value back.
    q: u8 = 16
    try:
        q **= 2
        assert False
    except OverflowError:
        pass
    assert q == 16
    print("native compound bitwise/shift/power ok")
