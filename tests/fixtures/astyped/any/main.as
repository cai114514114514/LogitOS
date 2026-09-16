# aether: 3.0
# Heterogeneous values explicitly opt into Any. Ordinary records and numbers
# remain native values; casts validate concrete type identity before reading.
from seq import unique
from test import assert_eq, failed

struct Record:
    label: str
    values: List[i64]

struct Link:
    tag: u8
    next: Any

struct PackedValues:
    tag: u8
    values: Array[f64, 2]

saved: Any = Any("module " + "box")


def choose(flag: bool) -> Any:
    if flag:
        return Any(7)
    return Any("chosen " + "text")


def recover[T](box: Any, exemplar: T) -> T:
    assert is_type[T](box)
    return cast[T](box)


def create_record() -> Any:
    record = Record("boxed " + "record", [2, 4, 6])
    return Any(record)


def check_values() -> None:
    assert is_type[str](saved)
    assert not is_type[i64](saved)
    assert cast[str](saved) == "module box"
    values: List[Any] = [Any(3), Any(2.5), Any("dynamic " + "text"), Any(true)]
    values.append(create_record())
    gc_collect()
    assert cast[i64](values[0]) == 3
    assert cast[f64](values[1]) == 2.5
    assert cast[str](values[2]) == "dynamic text"
    assert cast[bool](values[3])
    record = cast[Record](values[4])
    gc_collect()
    assert record.label == "boxed record" and record.values[2] == 6
    record.values.append(8)
    again = cast[Record](values[4])
    assert len(again.values) == 4
    words = Any(["first " + "word", "second"])
    assert cast[List[str]](words)[0] == "first word"
    assert recover(Any(i8(12)), i8(0)) == i8(12)
    assert recover(Any("泛型"), "") == "泛型"
    assert cast[i64](choose(true)) == 7
    assert cast[str](choose(false)) == "chosen text"
    assert is_type[Any](values[0])
    assert cast[i64](cast[Any](Any(values[0]))) == 3
    caught = 0
    try:
        cast[str](values[0])
    except TypeError as error:
        assert error.line > 0 and error.column > 0
        caught += 1
    try:
        cast[u64](values[0])
    except TypeError:
        caught += 1
    assert caught == 2


def clear_saved() -> None:
    global saved
    # Replace the only persistent box; only this scalar box remains after GC.
    saved = Any(0)


def check_equality() -> None:
    assert Any(7) == Any(7) and Any(7) != Any(8)
    assert Any(7) != Any(i8(7)) and Any(7) != Any(7.0)
    assert Any(true) != Any(1)
    assert Any("中" + "文") == Any("中文")
    assert Any("x\0y") != Any("x\0z")
    # Floating equality is numerical; comparing the payload bytes gets both
    # signed zero and NaN wrong, even when comparing a box with itself.
    assert Any(-0.0) == Any(0.0)
    assert Any(f32(-0.0)) == Any(f32(0.0))
    infinity = 1.0e308 * 1.0e308
    nan = Any(infinity - infinity)
    assert nan != nan
    assert Any(PackedValues(u8(4), [1.0, -0.0])) == Any(PackedValues(u8(4), [1.0, 0.0]))
    assert Any(PackedValues(u8(4), [1.0, 2.0])) != Any(PackedValues(u8(4), [1.0, 3.0]))

    items: List[Any] = [Any(7), Any("text"), Any(7)]
    assert Any(7) in items and Any(9) not in items
    assert len(unique(items)) == 2
    assert assert_eq(Any("a" + "b"), Any("ab"), "boxed string equality")
    assert failed() == 0
    assert Any(items) == Any(items) and Any(items) != Any([Any(7)])
    items.append(Any(items))
    assert Any(items) == Any(items)
    dictionary = {"key": items}
    assert Any(dictionary) == Any(dictionary)
    assert Any(dictionary) != Any({"key": items})

    # Value records compare their fields, including nested boxes, without
    # recursing on the C stack or inspecting struct padding.
    left = Any(0)
    right = Any(0)
    for i in range(2048):
        left = Any(Link(u8(1), left))
        right = Any(Link(u8(1), right))
    gc_collect()
    assert left == right
    assert Any(Link(u8(2), left)) != Any(Link(u8(1), right))


def main() -> None:
    gc_collect()
    check_values()
    check_equality()
    clear_saved()
    gc_collect()
    retained = gc_live_bytes()
    check_values_again = choose(true)
    assert cast[i64](check_values_again) == 7
    assert retained > 0 and retained < 128
    print("explicit Any ok")
