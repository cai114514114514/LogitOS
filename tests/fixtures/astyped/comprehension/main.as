# aether: 3.0
from seq import sorted
import seq

calls = 0


class Item:
    number: i64

    def get(self) -> i64:
        return self.number


def twice(value: i64) -> i64:
    return value * 2


def touch(value: i64) -> i64:
    global calls
    calls += 1
    gc_collect()
    return value * value


def copied[T](values: List[T]) -> List[T]:
    return [value for value in values]


def mapped(values: List[i64], function: Callable[[i64], i64]) -> List[i64]:
    return [function(value) for value in values]


def unwrap(values: List[Optional[i64]]) -> List[i64]:
    return [value + 1 for value in values if value != None]


def worker() -> None:
    value = "outer"
    squares = [touch(value) for value in range(5) if value % 2 == 0]
    assert len(squares) == 3 and squares[0] == 0 and squares[1] == 4 and squares[2] == 16
    assert value == "outer" and calls == 3
    assert [1 for unused in range(0)] != [1 for unused in range(0)]
    assert [sorted(3) for sorted in [twice]][0] == 6
    assert sorted([2, 1])[0] == 1
    assert [seq.get() for seq in [Item(4)]][0] == 4
    assert [calls for calls in range(2)][1] == 1
    assert calls == 3
    # The iterable sees the outer name. A nested comprehension may reuse it.
    rows = [[1, 2], [3, 4]]
    result = [[rows * 2 for rows in rows] for rows in rows]
    assert result[0][1] == 4 and result[1][0] == 6
    assert rows[0][1] == 2
    assert copied(["a", "b"])[1] == "b"
    assert mapped([2, 3], touch)[1] == 9
    assert calls == 5
    input: List[Optional[i64]] = [1, None, 3]
    present = unwrap(input)
    assert len(present) == 2 and present[1] == 4
    text = "A中文" + "Z"
    bytes = [byte for byte in text]
    gc_collect()
    assert "".join(bytes) == text
    keys = sorted([key for key in {"b": 2, "a": 1}])
    assert keys[0] == "a" and keys[1] == "b"
    array: Array[i64, 3] = [3, 4, 5]
    assert [x + 1 for x in array][2] == 6
    boxed: List[Any] = [Any(x) for x in range(2)]
    assert cast[i64](boxed[1]) == 1
    nullable: Optional[List[i64]] = [x for x in range(2)]
    if nullable != None:
        assert nullable[1] == 1
    before = calls
    try:
        broken = [touch(x) / (2 - x) for x in range(5)]
        assert false
    except ZeroDivisionError:
        assert calls == before + 3


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native comprehensions ok")
