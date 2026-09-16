# aether: 3.0
# Helpers over native dictionaries. Keys/values retain separate concrete
# layouts; explicit Any cells preserve the existing heterogeneous pair format.


def items[K: Hashable, V](d: Dict[K, V]) -> List[List[Any]]:
    out: List[List[Any]] = []
    for key in d:
        out.append([Any(key), Any(d[key])])
    return out


def keys[K: Hashable, V](d: Dict[K, V]) -> List[K]:
    return d.keys()


def values[K: Hashable, V](d: Dict[K, V]) -> List[V]:
    return d.values()


def has[K: Hashable, V](d: Dict[K, V], key: K) -> bool:
    return d.has(key)


def get[K: Hashable, V](d: Dict[K, V], key: K, default: V) -> V:
    return d.get(key, default)


def copy[K: Hashable, V](d: Dict[K, V]) -> Dict[K, V]:
    out: Dict[K, V] = {}
    for key in d:
        out[key] = d[key]
    return out


def merge[K: Hashable, V](a: Dict[K, V], b: Dict[K, V]) -> Dict[K, V]:
    # Later entries win; neither input is modified.
    out = copy(a)
    for key in b:
        out[key] = b[key]
    return out


def update[K: Hashable, V](target: Dict[K, V], patch: Dict[K, V]) -> Dict[K, V]:
    for key in patch:
        target[key] = patch[key]
    return target


def set_default[K: Hashable, V](d: Dict[K, V], key: K, value: V) -> V:
    if not d.has(key):
        d[key] = value
    return d[key]


def pop[K: Hashable, V](d: Dict[K, V], key: K, default: V) -> V:
    if d.has(key):
        value = d[key]
        d.remove(key)
        return value
    return default


def clear[K: Hashable, V](d: Dict[K, V]) -> Dict[K, V]:
    # Dictionary iteration owns a key snapshot; removal cannot skip entries.
    for key in d:
        d.remove(key)
    return d


def from_pairs[K: Hashable, V](pairs: List[List[Any]]) -> Dict[K, V]:
    # The caller's result annotation selects K and V. Each cell is checked;
    # malformed rows report IndexError or TypeError at the conversion site.
    out: Dict[K, V] = {}
    for pair in pairs:
        out[cast[K](pair[0])] = cast[V](pair[1])
    return out


def invert[K: Hashable, V: Hashable](d: Dict[K, V]) -> Dict[V, K]:
    out: Dict[V, K] = {}
    for key in d:
        out[d[key]] = key
    return out


def pick[K: Hashable, V](d: Dict[K, V], selected: List[K]) -> Dict[K, V]:
    out: Dict[K, V] = {}
    for key in selected:
        if d.has(key):
            out[key] = d[key]
    return out


def omit[K: Hashable, V](d: Dict[K, V], excluded: List[K]) -> Dict[K, V]:
    out: Dict[K, V] = {}
    for key in d:
        if not (key in excluded):
            out[key] = d[key]
    return out


def without[K: Hashable, V](d: Dict[K, V], key: K) -> Dict[K, V]:
    out = copy(d)
    out.remove(key)
    return out


def equal[K: Hashable, V: Equatable](a: Dict[K, V], b: Dict[K, V]) -> bool:
    if len(a) != len(b):
        return false
    for key in a:
        if not b.has(key) or a[key] != b[key]:
            return false
    return true


def frequencies[K: Hashable](xs: List[K]) -> Dict[K, i64]:
    out: Dict[K, i64] = {}
    for value in xs:
        out[value] = out.get(value, 0) + 1
    return out


def group_by[T, K: Hashable](f: Callable[[T], K], xs: List[T]) -> Dict[K, List[T]]:
    out: Dict[K, List[T]] = {}
    for value in xs:
        key = f(value)
        if not out.has(key):
            out[key] = []
        out[key].append(value)
    return out
