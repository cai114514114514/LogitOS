# aether: 3.0
# Deterministic, non-cryptographic generator. Preserve the original 31-bit LCG
# sequence so seeds remain reproducible across the native migration.
_MOD: i64 = 2147483648
_state: i64 = 1


def seed(n: i64) -> i64:
    global _state
    normalized = n % _MOD
    if normalized < 0:
        normalized += _MOD
    if normalized == 0:
        normalized = 1
    _state = normalized
    return normalized


def _next_raw() -> i64:
    global _state
    # The largest intermediate is below 2^62. Checked i64 arithmetic preserves
    # the old sequence without hiding overflow behind VM promotion.
    _state = (1103515245 * _state + 12345) % _MOD
    return _state


def random() -> f64:
    return f64(_next_raw()) / f64(_MOD)


def randint(lo: i64, hi: i64) -> i64:
    if hi < lo:
        raise ValueError("randint() needs hi >= lo")
    # Preserve the inclusive interval and modulo mapping. This is not an
    # unbiased sampler; changing the mapping would change existing seeded runs.
    span = hi - lo + 1
    return lo + _next_raw() % span


def randrange(start: i64, stop: i64) -> i64:
    if stop <= start:
        raise ValueError("randrange() needs stop > start")
    return start + _next_raw() % (stop - start)


def bool() -> bool:
    return (_next_raw() & 1) == 1


def choice[T](xs: List[T]) -> T:
    if len(xs) == 0:
        raise ValueError("choice() needs a non-empty list")
    return xs[randrange(0, len(xs))]


def shuffle[T](xs: List[T]) -> List[T]:
    # Return a separate list and preserve the descending Fisher-Yates draw
    # order. T stays in its concrete native representation.
    out: List[T] = []
    for item in xs:
        out.append(item)
    index = len(out) - 1
    while index > 0:
        selected = randint(0, index)
        saved = out[index]
        out[index] = out[selected]
        out[selected] = saved
        index -= 1
    return out


def sample[T](xs: List[T], n: i64) -> List[T]:
    if n < 0 or n > len(xs):
        raise ValueError("sample() size out of range")
    shuffled = shuffle(xs)
    out: List[T] = []
    for index in range(n):
        out.append(shuffled[index])
    return out
