# aether: 3.0
# Dictionary values remain native records. Keys are compared by value, and
# loops use snapshots so removing or adding entries cannot invalidate a cursor.
struct Entry:
    name: str
    numbers: List[i64]


def create() -> Dict[str, Entry]:
    data: Dict[str, Entry] = {}
    for i in range(80):
        data["key " + str(i)] = Entry("value " + str(i), [i, i * 2])
    return data


def lookup[K: Hashable, V](data: Dict[K, V], key: K, default: V) -> V:
    return data.get(key, default)


def same[K: Hashable](a: K, b: K) -> bool:
    return a == b


def default_value(data: Dict[str, i64]) -> i64:
    data["target"] = 42
    # Growth in an eager default argument must precede resolving the bucket.
    for i in range(80):
        data[str(i)] = i
    return 9


def check_scalars() -> None:
    data = {"a": 1, "b": 2, "a": 3}
    assert len(data) == 2 and data["a"] == 3
    data["a"] += 4
    assert data["a"] == 7
    assert "a" in data and not ("missing" in data)
    assert data.has("b") and not data.has("missing")
    assert lookup(data, "missing", 8) == 8
    assert lookup(data, "a", 8) == 7
    assert data.remove("b") and not data.remove("b")
    assert data.get("target", default_value(data)) == 42
    small: Dict[u8, str] = {1: "one", 255: "last"}
    assert small[255] == "last" and 1 in small
    flags = {true: 1, false: 2}
    assert flags[false] == 2 and same(true, true)
    assert same("中文", "中文") and not same(i8(1), i8(2))
    wide: Dict[u64, i64] = {18446744073709551615: 9}
    assert wide[18446744073709551615] == 9
    signed = {-9223372036854775808: 1, 9223372036854775807: 2}
    assert signed[-9223372036854775808] == 1
    binary = {"a\0b": 1, "a\0c": 2, "": 3}
    assert len(binary) == 3 and binary["a\0b"] == 1
    assert binary["a\0c"] == 2 and binary[""] == 3
    caught = 0
    try:
        data["missing"]
    except KeyError as error:
        assert error.code == 11 and error.line > 0 and error.column > 0
        caught += 1
    try:
        data["missing"] += 1
    except KeyError:
        caught += 1
    assert caught == 2


def check_roots() -> None:
    data = create()
    gc_collect()
    assert len(data) == 80
    for i in range(80):
        entry = data["key " + str(i)]
        assert entry.name == "value " + str(i)
        assert entry.numbers[1] == i * 2
    data["key 0"].numbers.append(7)
    data["key 0"].name = "changed " + "name"
    gc_collect()
    assert data["key 0"].name == "changed name"
    assert data["key 0"].numbers[2] == 7
    keys = data.keys()
    values = data.values()
    visited = 0
    for key in data:
        assert data.remove(key)
        data["new " + str(visited)] = Entry("new", [])
        gc_collect()
        visited += 1
    assert visited == 80 and len(data) == 80
    gc_collect()
    assert len(keys) == 80 and len(values) == 80
    total = 0
    for entry in values:
        total += entry.numbers[0]
    assert total == 3160
    for key in keys:
        assert key.find("key ") == 0
    # Repeated deletion/reinsertion exercises tombstones and same-size rehash.
    for cycle in range(4):
        for key in data:
            assert data.remove(key)
        assert len(data) == 0
        for i in range(80):
            data[str(i)] = Entry("again", [i])
    for i in range(80):
        assert data[str(i)].numbers[0] == i


def main() -> None:
    check_scalars()
    check_roots()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native dictionary ok")
