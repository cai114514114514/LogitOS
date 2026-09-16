# aether: 3.0
import sets


def fresh[K: Hashable]() -> Dict[K, bool]:
    # A generic result context can flow through another generic function.
    return sets.empty()


def exercise() -> None:
    empty: Dict[str, bool] = sets.empty()
    assert sets.size(empty) == 0 and len(sets.to_list(empty)) == 0
    assert sets.equal(empty, empty) and sets.disjoint(empty, empty)
    a = sets.from_list(["a", "b", "a"])
    b = sets.from_list(["b", "c"])
    assert sets.size(a) == 2
    assert sets.contains(a, "a") and not sets.contains(a, "c")
    copied = sets.copy(a)
    added = sets.add(copied, "c")
    assert sets.contains(copied, "c") and sets.contains(added, "c")
    assert not sets.contains(a, "c")
    assert sets.remove(copied, "c") and not sets.remove(copied, "c")
    assert sets.equal(copied, a)
    assert sets.equal(sets.union(a, b), sets.from_list(["a", "b", "c"]))
    assert sets.equal(sets.intersection(a, b), sets.from_list(["b"]))
    assert sets.equal(sets.difference(a, b), sets.from_list(["a"]))
    assert sets.equal(sets.symmetric_difference(a, b), sets.from_list(["a", "c"]))
    assert sets.is_subset(a, sets.union(a, b))
    assert not sets.is_subset(a, b)
    assert sets.is_superset(sets.union(a, b), b)
    assert not sets.is_superset(a, b)
    assert sets.disjoint(sets.difference(a, b), b)
    assert not sets.disjoint(a, b)
    assert not sets.equal(a, b)
    assert not sets.equal(a, empty)
    assert sets.is_subset(empty, a)
    assert sets.equal(sets.union(empty, a), a)
    keys = sets.to_list(a)
    assert len(keys) == 2
    for key in keys:
        assert sets.contains(a, key)
    numbers: Dict[i64, bool] = fresh()
    sets.add(numbers, -9223372036854775808)
    sets.add(numbers, 9223372036854775807)
    gc_collect()
    assert sets.size(numbers) == 2
    flags = sets.from_list([true, false, true])
    assert sets.size(flags) == 2
    assert sets.contains(flags, false)
    small = sets.from_list([u8(0), u8(255)])
    assert sets.contains(small, u8(255))


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native sets library ok")
