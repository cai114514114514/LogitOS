# aether: 3.0
struct Record:
    flag: bool
    byte: u8
    number: i64
    text: str
    values: List[i64]


def function(value: i64) -> i64:
    return value


def view_text(values: Slice[str]) -> str:
    return str(values)


def create() -> Any:
    return Any(Record(true, u8(255), -9223372036854775808, "中文 " + "text", [1, 2]))


def marker() -> i64:
    print("argument first")
    return 2


def exercise() -> None:
    assert str([1, 2, 3]) == "[1, 2, 3]"
    assert str([i8(-128), i8(127)]) == "[-128, 127]"
    wide: u64 = 18446744073709551615
    assert str([wide]) == "[18446744073709551615]"
    assert str([1.25, -2.5]) == "[1.25, -2.5]"
    assert str([f32(1.5)]) == "[1.5]"
    assert str([true, false]) == "[true, false]"
    assert str(["a", "b"]) == "['a', 'b']"
    assert str(["a\0b"]) == "['a\0b']"
    assert str([[1], [2, 3]]) == "[[1], [2, 3]]"
    assert str({"key": [1, 2]}) == "{'key': [1, 2]}"
    assert str({1: "number"}) == "{1: 'number'}"
    empty: Dict[str, i64] = {}
    assert str(empty) == "{}"
    array: Array[str, 2] = ["first", "second"]
    assert str(array) == "['first', 'second']"
    assert view_text(array) == "['first', 'second']"
    assert str(Any("unquoted")) == "unquoted"
    assert str([Any("quoted"), Any(7)]) == "['quoted', 7]"
    assert str(function) == "<fn>"
    assert str(Any(function)) == "<fn>"
    assert str(ValueError("message")) == "ValueError: message"
    assert str(Any(IOError("file"))) == "IOError: file"
    boxed = create()
    gc_collect()
    expected = "Record(flag=true, byte=255, number=-9223372036854775808, text='中文 text', values=[1, 2])"
    assert str(boxed) == expected
    many: List[i64] = []
    for i in range(1000):
        many.append(i)
    formatted = str(many)
    assert len(formatted) > 4000
    assert formatted.find("998, 999]") > 0
    cycle: List[Any] = []
    cycle.append(Any(cycle))
    recursive = str(cycle)
    assert recursive.find("[...]") >= 0 and len(recursive) < 100
    graph: Dict[str, Any] = {}
    graph["self"] = Any(graph)
    assert str(graph).find("{...}") >= 0
    print([1], marker())
    print(Any("dynamic"), {"answer": 42}, [true, false])


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native formatting ok")
