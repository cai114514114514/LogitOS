# aether: 3.0
# Sets use native Dict keys with a bool marker. Operations returning a new set
# copy membership; add/remove alone mutate their input. Order is unspecified.

def empty[K: Hashable]() -> Dict[K, bool]:
    # The result annotation at the call site determines K for this empty set.
    return {}


def from_list[K: Hashable](xs: List[K]) -> Dict[K, bool]:
    out: Dict[K, bool] = {}
    for value in xs:
        out[value] = true
    return out


def to_list[K: Hashable](s: Dict[K, bool]) -> List[K]:
    return s.keys()


def size[K: Hashable](s: Dict[K, bool]) -> i64:
    return len(s)


def contains[K: Hashable](s: Dict[K, bool], value: K) -> bool:
    return s.has(value)


def add[K: Hashable](s: Dict[K, bool], value: K) -> Dict[K, bool]:
    s[value] = true
    return s


def remove[K: Hashable](s: Dict[K, bool], value: K) -> bool:
    return s.remove(value)


def copy[K: Hashable](s: Dict[K, bool]) -> Dict[K, bool]:
    out: Dict[K, bool] = {}
    for value in s:
        out[value] = true
    return out


def union[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> Dict[K, bool]:
    out = copy(a)
    for value in b:
        out[value] = true
    return out


def intersection[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> Dict[K, bool]:
    out: Dict[K, bool] = {}
    for value in a:
        if b.has(value):
            out[value] = true
    return out


def difference[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> Dict[K, bool]:
    out: Dict[K, bool] = {}
    for value in a:
        if not b.has(value):
            out[value] = true
    return out


def symmetric_difference[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> Dict[K, bool]:
    out: Dict[K, bool] = {}
    for value in a:
        if not b.has(value):
            out[value] = true
    for value in b:
        if not a.has(value):
            out[value] = true
    return out


def is_subset[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> bool:
    for value in a:
        if not b.has(value):
            return false
    return true


def is_superset[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> bool:
    return is_subset(b, a)


def disjoint[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> bool:
    for value in a:
        if b.has(value):
            return false
    return true


def equal[K: Hashable](a: Dict[K, bool], b: Dict[K, bool]) -> bool:
    return len(a) == len(b) and is_subset(a, b)
