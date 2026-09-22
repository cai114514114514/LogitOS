# aether: 3.0
# A mixed expression converts values, never the storage they were loaded from.
# Typed helper parameters make an accidental change of result/storage type fail
# checking; the large unsigned value catches using sitofp for every integer.
events: List[i64] = []
values: List[f64] = [1.5]

def expect64(actual: f64, expected: f64) -> None:
    assert actual == expected

def expect32(actual: f32, expected: f32) -> None:
    assert actual == expected

def integer(value: i64) -> i64:
    return value

def real(value: f64) -> f64:
    return value

def mark_integer() -> i64:
    events.append(1)
    return -2

def mark_real() -> f64:
    events.append(2)
    return 0.5

def choose_values() -> List[f64]:
    events.append(3)
    return values

def choose_index() -> i64:
    events.append(4)
    return 0

def change_values() -> i64:
    events.append(5)
    # The previous left value must be captured before evaluating the right.
    values[0] = 99.0
    return 2

def main() -> None:
    number = 2
    fraction = 1.5
    expect64(1 + 2.5, 3.5)
    expect64(2.5 + 1, 3.5)
    expect64(number + fraction, 3.5)
    expect64(fraction + number, 3.5)
    expect64(number - fraction, 0.5)
    expect64(fraction - number, -0.5)
    expect64(number * fraction, 3.0)
    expect64(fraction * number, 3.0)
    expect64(number / fraction, 2.0 / 1.5)
    expect64(fraction / number, 0.75)
    expect64(number % fraction, 0.5)
    expect64(fraction % number, 1.5)
    expect64(number ** fraction, 2.0 ** 1.5)
    expect64(fraction ** number, 2.25)
    expect64(2 ** -2.0, 0.25)
    expect64(2.0 ** -2, 0.25)
    expect64(real(integer(number) + real(fraction)) * integer(2), 7.0)
    assert integer(number) == 2
    assert f"{number + fraction} {fraction + number}" == "3.5 3.5"

    # Both operand orders exercise all six comparison predicates. Equality
    # cases also prove conversion happens before comparison, not on its bool.
    assert number > fraction and fraction < number
    assert number >= fraction and fraction <= number
    assert not (number < fraction) and not (fraction > number)
    assert not (number <= fraction) and not (fraction >= number)
    assert number != fraction and fraction != number
    assert not (number == fraction) and not (fraction == number)
    assert number == 2.0 and 2.0 == number
    assert number <= 2.0 and 2.0 >= number
    assert number >= 2.0 and 2.0 <= number

    signed8: i8 = -7
    signed16: i16 = -7
    signed32: i32 = -7
    signed64: i64 = -7
    unsigned8: u8 = 255
    unsigned16: u16 = 65535
    unsigned32: u32 = 4294967295
    unsigned64: u64 = 18446744073709551615
    expect64(signed8 + 0.5, -6.5)
    expect64(0.5 + signed16, -6.5)
    expect64(signed32 + 0.5, -6.5)
    expect64(0.5 + signed64, -6.5)
    expect64(unsigned8 + 0.5, 255.5)
    expect64(0.5 + unsigned16, 65535.5)
    expect64(unsigned32 + 0.5, 4294967295.5)
    expect64(0.0 + unsigned64, 18446744073709551616.0)
    assert unsigned64 > 9223372036854775808.0
    assert 9223372036854775808.0 < unsigned64
    assert unsigned64 == 18446744073709551616.0
    assert unsigned64 == 18446744073709551615
    assert signed8 == i8(-7)

    small: f32 = 1.5
    wide: f64 = 0.25
    expect32(small + number, 3.5)
    expect32(number + small, 3.5)
    inferred = small + number
    expect32(inferred, 3.5)
    expect32(small - number, -0.5)
    expect32(number - small, 0.5)
    expect32(small * number, 3.0)
    expect32(number * small, 3.0)
    expect32(small / number, 0.75)
    expect32(number / small, f32(2.0 / 1.5))
    expect32(small % number, 1.5)
    expect32(number % small, 0.5)
    expect32(small ** number, 2.25)
    expect32(number ** small, f32(2.0 ** 1.5))
    expect64(small + wide, 1.75)
    expect64(wide + small, 1.75)
    expect32(small, 1.5)
    rounded: f32 = 16777216.0
    # f32 + integer remains f32, so the low bit rounds away at the operation.
    expect32(rounded + integer(1), 16777216.0)
    expect32(integer(1) + rounded, 16777216.0)
    expect64(rounded + real(1.0), 16777217.0)

    fraction += number
    fraction -= number
    fraction *= number
    fraction /= number
    fraction %= number
    expect64(fraction, 1.5)
    small += number
    small -= number
    small *= number
    small /= number
    small %= number
    expect32(small, 1.5)
    expect64(mark_integer() + mark_real(), -1.5)
    expect64(mark_real() - mark_integer(), 2.5)
    assert len(events) == 4
    assert events[0] == 1 and events[1] == 2
    assert events[2] == 2 and events[3] == 1
    choose_values()[choose_index()] += change_values()
    expect64(values[0], 3.5)
    assert len(events) == 7
    assert events[4] == 3 and events[5] == 4 and events[6] == 5
    print("mixed numeric ok")
