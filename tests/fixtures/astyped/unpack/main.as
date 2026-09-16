# aether: 3.0

first = 1
second = 2
calls = 0


def collect() -> i64:
    gc_collect()
    return 42


def fail() -> i64:
    raise ValueError("second value failed")


def source() -> List[i64]:
    global calls
    calls += 1
    return [4, 5]


def swap_globals() -> None:
    global first, second
    first, second = second, first


def from_slice(values: Slice[i64]) -> i64:
    a, b = values
    return a * 10 + b


def worker() -> None:
    a, b = 1, 2
    a, b = b, a
    assert a == 2 and b == 1
    same, same = 3, 4
    assert same == 4
    number, text, real = 3, "中文", 1.5
    assert number == 3 and text == "中文" and real == 1.5
    small: u8 = 0
    small, number = 255, 4
    assert small == 255 and number == 4
    a, b = source()
    assert a == 4 and b == 5 and calls == 1
    a, b = range(7, 9)
    assert a == 7 and b == 8
    array: Array[i64, 2] = [8, 9]
    a, b = array
    assert a == 8 and b == 9 and from_slice(array) == 89
    left, right = "X" + "Y"
    gc_collect()
    assert left == "X" and right == "Y"
    optional_a: Optional[i64] = None
    optional_b: Optional[i64] = None
    optional_a, optional_b = [1, 2]
    assert optional_a != None and optional_b != None
    assert optional_a + optional_b == 3
    text, number = "rooted " + "right side", collect()
    assert text == "rooted right side" and number == 42
    try:
        a, b = 100, fail()
        assert false
    except ValueError:
        assert a == 8 and b == 9
    try:
        a, b = [100]
        assert false
    except ValueError:
        assert a == 8 and b == 9
    try:
        a, b = [1, 2, 3]
        assert false
    except ValueError:
        assert a == 8 and b == 9
    swap_globals()
    assert first == 2 and second == 1


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native multiple assignment ok")
