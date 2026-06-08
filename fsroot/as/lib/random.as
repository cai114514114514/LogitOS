# random -- deterministic pseudo-random helpers
#   seed(n) for repeatable scripts; not cryptographic.

_MOD = 2147483648
_state = [1]

def seed(n):
    n = n % _MOD
    if n < 0:
        n = n + _MOD
    if n == 0:
        n = 1
    _state[0] = n
    return n

def _next_raw():
    _state[0] = (1103515245 * _state[0] + 12345) % _MOD
    return _state[0]

def random():
    return _next_raw() * 1.0 / _MOD

def randint(lo, hi):                # inclusive
    if hi < lo:
        raise "randint() needs hi >= lo"
    span = hi - lo + 1
    return lo + (_next_raw() % span)

def randrange(start, stop):
    if stop <= start:
        raise "randrange() needs stop > start"
    return start + (_next_raw() % (stop - start))

def bool():
    return (_next_raw() & 1) == 1

def choice(xs):
    if len(xs) == 0:
        raise "choice() needs a non-empty list"
    return xs[randrange(0, len(xs))]

def shuffle(xs):
    out = []
    for x in xs:
        out.append(x)
    i = len(out) - 1
    while i > 0:
        j = randint(0, i)
        t = out[i]
        out[i] = out[j]
        out[j] = t
        i = i - 1
    return out

def sample(xs, n):
    if n < 0 or n > len(xs):
        raise "sample() size out of range"
    shuffled = shuffle(xs)
    out = []
    i = 0
    while i < n:
        out.append(shuffled[i])
        i = i + 1
    return out
