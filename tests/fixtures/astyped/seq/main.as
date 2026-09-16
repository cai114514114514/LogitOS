# aether: 3.0
import seq

struct Item:
    key: i64
    order: i64

calls: i64 = 0


def same[T: Equatable](a: List[T], b: List[T]) -> None:
    assert len(a) == len(b)
    index = 0
    while index < len(a):
        assert a[index] == b[index]
        index += 1


def even(value: i64) -> bool:
    global calls
    calls += 1
    return value % 2 == 0


def identity[T](value: T) -> T:
    return value


def twice(value: i64) -> i64:
    return value * 2


def add(total: i64, value: i64) -> i64:
    return total + value


def visit(value: i64) -> None:
    global calls
    calls += value


def less(a: Item, b: Item) -> bool:
    gc_collect()
    return a.key < b.key


def slice_has(values: Slice[i64], value: i64) -> bool:
    return value in values


def exercise() -> None:
    global calls
    values = [1, 2, 3, 2]
    empty: List[i64] = []
    copied = seq.copy(values)
    copied[0] = 9
    assert values[0] == 1 and copied[0] == 9
    assert seq.is_empty(empty) and not seq.is_empty(values)
    assert seq.first(values) == 1 and seq.last(values) == 2
    same(seq.map(twice, values), [2, 4, 6, 4])
    same(seq.map(identity, values), values)
    same(seq.filter(even, values), [2, 2])
    same(seq.reject(even, values), [1, 3])
    partitioned = seq.partition(even, values)
    same(partitioned[0], [2, 2])
    same(partitioned[1], [1, 3])
    assert seq.reduce(add, values, 10) == 18
    assert seq.reduce(add, empty, 10) == 10
    calls = 0
    seq.foreach(visit, values)
    assert calls == 8
    seq.foreach(twice, values)
    assert calls == 8
    same(seq.concat([1, 2], [3]), [1, 2, 3])
    same(seq.flatten1([[1, 2], [], [3]]), [1, 2, 3])
    assert seq.sum(values) == 8 and seq.sum(empty) == 0
    assert seq.maxl(values) == 3 and seq.minl(values) == 1
    assert seq.maxl(["b", "a"]) == "b"
    calls = 0
    assert seq.any(even, [1, 2, 4]) and calls == 2
    calls = 0
    assert not seq.all(even, [2, 1, 4]) and calls == 2
    calls = 0
    assert seq.find(even, [1, 2, 4]) == 1 and calls == 2
    assert not seq.any(even, empty) and seq.all(even, empty)
    assert seq.find(even, [1, 3]) == -1
    assert seq.index_of(values, 2) == 1 and seq.index_of(values, 9) == -1
    assert seq.last_index_of(values, 2) == 3 and seq.last_index_of(values, 9) == -1
    assert seq.count(values, 2) == 2 and seq.count_if(even, values) == 2
    assert seq.contains(values, 3) and not seq.contains(empty, 3)
    same(seq.unique(values), [1, 2, 3])
    same(seq.reverse(values), [2, 3, 2, 1])
    same(seq.sorted(values), [1, 2, 2, 3])
    same(seq.sorted(["中文", "a", "b"]), ["a", "b", "中文"])
    same(seq.sorted([1.5, 0.5, 2.5]), [0.5, 1.5, 2.5])
    records = [Item(2, 0), Item(1, 1), Item(2, 2)]
    ordered = seq.sorted_by(records, less)
    assert ordered[0].order == 1 and ordered[1].order == 0 and ordered[2].order == 2
    assert records[0].order == 0
    zipped = seq.zip([1, 2, 3], ["a", "b"])
    assert len(zipped) == 2 and cast[i64](zipped[1][0]) == 2
    assert cast[str](zipped[1][1]) == "b"
    numbered = seq.enumerate(["a " + "word", "second"])
    gc_collect()
    assert cast[i64](numbered[0][0]) == 0
    assert cast[str](numbered[0][1]) == "a word"
    same(seq.take(values, 2), [1, 2])
    assert len(seq.take(values, -1)) == 0
    same(seq.drop(values, 2), [3, 2])
    same(seq.drop(values, -1), values)
    same(seq.slice(values, -3, -1), [2, 3])
    same(seq.slice(values, -99, 99), values)
    assert len(seq.slice(values, 8, 3)) == 0
    same(seq.repeat(7, 3), [7, 7, 7])
    assert len(seq.repeat(7, -1)) == 0
    chunks = seq.chunk(values, 3)
    same(chunks[0], [1, 2, 3])
    same(chunks[1], [2])
    assert len(seq.chunk(empty, 1)) == 0
    # Heap collections compare identity, matching the original language.
    row = [1]
    same_row = row
    assert row == same_row and row != [1]
    assert len(seq.unique([row, same_row, [1]])) == 2
    assert row in [row] and not (row in [[1]])
    dict = {"x": 1}
    assert dict in [dict] and not (dict in [{"x": 1}])
    assert twice in [twice] and twice != identity
    assert len(seq.unique([twice, twice])) == 1
    assert 1.5 in [0.5, 1.5] and not (2.5 in [0.5, 1.5])
    array: Array[i64, 3] = [1, 2, 3]
    assert 2 in array and not (9 in array)
    assert slice_has(array, 3) and not slice_has(array, 9)
    caught = 0
    try:
        seq.first(empty)
    except ValueError:
        caught += 1
    try:
        seq.last(empty)
    except ValueError:
        caught += 1
    try:
        seq.chunk(values, 0)
    except ValueError:
        caught += 1
    try:
        seq.maxl(empty)
    except IndexError:
        caught += 1
    assert caught == 4


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native sequence library ok")
