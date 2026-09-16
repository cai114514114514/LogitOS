# aether: 3.0
import dicts

calls: i64 = 0


def parity(value: i64) -> i64:
    global calls
    calls += 1
    gc_collect()
    return value % 2


def initial(value: str) -> str:
    gc_collect()
    return value[0]


def identity[K: Hashable](value: K) -> K:
    return value


def exercise() -> None:
    data = {"a": 1, "b": 2}
    assert len(dicts.keys(data)) == 2 and len(dicts.values(data)) == 2
    for value in dicts.values(data):
        assert value == 1 or value == 2
    assert dicts.has(data, "a") and not dicts.has(data, "c")
    assert dicts.get(data, "a", 9) == 1 and dicts.get(data, "c", 9) == 9
    copied = dicts.copy(data)
    assert dicts.equal(copied, data)
    copied["a"] = 8
    assert data["a"] == 1 and not dicts.equal(copied, data)
    merged = dicts.merge(data, {"b": 9, "c": 3})
    assert merged["b"] == 9 and merged["c"] == 3 and data["b"] == 2
    updated = dicts.update(copied, {"d": 4})
    assert updated == copied and copied["d"] == 4
    assert dicts.set_default(copied, "a", 99) == 8
    assert dicts.set_default(copied, "new", 7) == 7
    assert dicts.pop(copied, "new", 0) == 7
    assert dicts.pop(copied, "new", 0) == 0
    assert not copied.has("new")
    assert dicts.clear(copied) == copied and len(copied) == 0
    rows = dicts.items(data)
    restored: Dict[str, i64] = dicts.from_pairs(rows)
    assert dicts.equal(restored, data)
    inverted = dicts.invert(data)
    assert inverted[1] == "a" and inverted[2] == "b"
    assert dicts.equal(dicts.pick(data, ["a", "missing"]), {"a": 1})
    assert dicts.equal(dicts.omit(data, ["a", "missing"]), {"b": 2})
    assert dicts.equal(dicts.without(data, "a"), {"b": 2})
    assert data.has("a")
    assert not dicts.equal(data, {"a": 1})
    assert not dicts.equal(data, {"a": 1, "c": 2})
    assert dicts.frequencies(["a", "b", "a"])["a"] == 2
    grouped = dicts.group_by(parity, [1, 2, 3, 4])
    assert calls == 4
    assert grouped[0][0] == 2 and grouped[0][1] == 4
    assert grouped[1][0] == 1 and grouped[1][1] == 3
    words = dicts.group_by(initial, ["a" + "word", "another", "beta"])
    gc_collect()
    assert words["a"][0] == "aword" and words["a"][1] == "another"
    assert words["b"][0] == "beta"
    repeated = dicts.group_by(identity, [1, 2, 1])
    assert len(repeated[1]) == 2
    empty: List[i64] = []
    assert len(dicts.frequencies(empty)) == 0
    assert len(dicts.group_by(parity, empty)) == 0
    assert calls == 4
    # Pair conversions retain integer precision and report their real failure.
    pairs: List[List[Any]] = [[Any("wide"), Any(9223372036854775807)]]
    wide: Dict[str, i64] = dicts.from_pairs(pairs)
    assert wide["wide"] == 9223372036854775807
    caught = 0
    try:
        wrong: Dict[i64, i64] = dicts.from_pairs(pairs)
    except TypeError as error:
        assert error.line > 0 and error.file.find("dicts.as") >= 0
        caught += 1
    try:
        short: List[List[Any]] = [[Any("key")]]
        wrong_row: Dict[str, i64] = dicts.from_pairs(short)
    except IndexError:
        caught += 1
    assert caught == 2


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native dictionary library ok")
