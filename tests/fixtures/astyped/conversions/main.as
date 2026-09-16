# aether: 3.0

def stringify[T: Number](value: T) -> str:
    return str(value)


def valid() -> None:
    assert str(-9223372036854775808) == "-9223372036854775808"
    maximum: u64 = 18446744073709551615
    assert str(maximum) == "18446744073709551615"
    assert str(i8(-12)) == "-12"
    assert str(2.5) == "2.5"
    assert str(true) == "true" and str(false) == "false"
    assert str("中文") == "中文"
    assert stringify(i32(42)) == "42"
    assert parse_int("010") == 10
    assert parse_int("+0x7f") == 127
    assert parse_int("-0x8000000000000000") == -9223372036854775808
    assert parse_float("1.25garbage".slice(0, 4)) == 1.25
    assert parse_float("-1.5e2") == -150.0
    assert parse_float(".125") == 0.125
    assert parse_float(str(0.1)) == 0.1
    assert ord(chr(0)) == 0 and len(chr(0)) == 1
    assert ord(chr(255)) == 255
    assert ord("中文") == 228
    assert f64bits(1.0) == 4607182418800017408
    assert f64bits(-0.0) == -9223372036854775808
    assert f64bits(1) == f64bits(1.0)
    text = str(9223372036854775807) + chr(33)
    gc_collect()
    assert text == "9223372036854775807!"


def invalid() -> None:
    caught = 0
    try:
        parse_int("9223372036854775808")
    except ConversionError:
        caught += 1
    try:
        parse_int("1x")
    except ConversionError:
        caught += 1
    try:
        parse_int("")
    except ConversionError:
        caught += 1
    try:
        parse_float("1.25garbage")
    except ConversionError:
        caught += 1
    try:
        parse_float("bad")
    except ConversionError:
        caught += 1
    try:
        parse_float("1\0more")
    except ConversionError:
        caught += 1
    try:
        ord("")
    except ValueError:
        caught += 1
    try:
        chr(-1)
    except ValueError:
        caught += 1
    try:
        chr(256)
    except ValueError:
        caught += 1
    assert caught == 9


def main() -> None:
    valid()
    invalid()
    gc_collect()
    assert gc_live_bytes() == 0
    print("scalar conversions ok")
