# seq -- functional helpers over lists (pure AetherScript stdlib)
#   from seq import map, filter, reduce, sorted, zip, enumerate

def copy(xs):
    out = []
    for x in xs:
        out.append(x)
    return out

def is_empty(xs):
    return len(xs) == 0

def first(xs):
    if len(xs) == 0:
        raise "first() needs a non-empty list"
    return xs[0]

def last(xs):
    if len(xs) == 0:
        raise "last() needs a non-empty list"
    return xs[len(xs) - 1]

def map(f, xs):
    out = []
    for x in xs:
        out.append(f(x))
    return out

def filter(f, xs):
    out = []
    for x in xs:
        if f(x):
            out.append(x)
    return out

def reject(f, xs):
    out = []
    for x in xs:
        if not f(x):
            out.append(x)
    return out

def partition(f, xs):               # [matches, misses]
    yes = []
    no = []
    for x in xs:
        if f(x):
            yes.append(x)
        else:
            no.append(x)
    return [yes, no]

def reduce(f, xs, init):
    acc = init
    for x in xs:
        acc = f(acc, x)
    return acc

def foreach(f, xs):
    for x in xs:
        f(x)
    return

def concat(a, b):
    out = copy(a)
    for x in b:
        out.append(x)
    return out

def flatten1(xss):
    out = []
    for xs in xss:
        for x in xs:
            out.append(x)
    return out

def sum(xs):
    s = 0
    for x in xs:
        s = s + x
    return s

def maxl(xs):                       # max of a non-empty list
    m = xs[0]
    for x in xs:
        if x > m:
            m = x
    return m

def minl(xs):
    m = xs[0]
    for x in xs:
        if x < m:
            m = x
    return m

def any(f, xs):                     # short-circuits (no break -> loop guard)
    r = false
    i = 0
    while i < len(xs) and not r:
        if f(xs[i]):
            r = true
        i = i + 1
    return r

def all(f, xs):
    r = true
    i = 0
    while i < len(xs) and r:
        if not f(xs[i]):
            r = false
        i = i + 1
    return r

def find(f, xs):                    # index of the first match, or -1
    i = 0
    res = -1
    while i < len(xs) and res < 0:
        if f(xs[i]):
            res = i
        i = i + 1
    return res

def index_of(xs, v):
    i = 0
    while i < len(xs):
        if xs[i] == v:
            return i
        i = i + 1
    return -1

def last_index_of(xs, v):
    i = len(xs) - 1
    while i >= 0:
        if xs[i] == v:
            return i
        i = i - 1
    return -1

def count(xs, v):
    n = 0
    for x in xs:
        if x == v:
            n = n + 1
    return n

def count_if(f, xs):
    n = 0
    for x in xs:
        if f(x):
            n = n + 1
    return n

def contains(xs, v):
    return v in xs

def unique(xs):
    out = []
    for x in xs:
        if not (x in out):
            out.append(x)
    return out

def reverse(xs):
    out = []
    i = len(xs) - 1
    while i >= 0:
        out.append(xs[i])
        i = i - 1
    return out

def sorted_by(xs, less):            # comparator less(a, b) -> bool; stable insertion sort
    out = []
    for x in xs:
        out.append(x)
    i = 1
    while i < len(out):
        key = out[i]
        j = i - 1
        while j >= 0 and less(key, out[j]):
            out[j + 1] = out[j]
            j = j - 1
        out[j + 1] = key
        i = i + 1
    return out

def _asc(a, b):
    return a < b

def sorted(xs):                     # ascending (numbers / comparables)
    return sorted_by(xs, _asc)

def zip(a, b):                      # [[a[i], b[i]], ...], length = min(len a, len b)
    n = len(a)
    if len(b) < n:
        n = len(b)
    out = []
    i = 0
    while i < n:
        out.append([a[i], b[i]])
        i = i + 1
    return out

def enumerate(xs):                  # [[0, xs[0]], [1, xs[1]], ...]
    out = []
    i = 0
    while i < len(xs):
        out.append([i, xs[i]])
        i = i + 1
    return out

def take(xs, n):
    out = []
    i = 0
    while i < n and i < len(xs):
        out.append(xs[i])
        i = i + 1
    return out

def drop(xs, n):
    out = []
    i = n
    if i < 0:                           # negative n drops nothing (mirrors take + slice clamping)
        i = 0
    while i < len(xs):
        out.append(xs[i])
        i = i + 1
    return out

def slice(xs, start, stop):
    out = []
    i = start
    if i < 0:
        i = len(xs) + i
    if stop < 0:
        stop = len(xs) + stop
    if i < 0:
        i = 0
    if stop > len(xs):
        stop = len(xs)
    while i < stop:
        out.append(xs[i])
        i = i + 1
    return out

def repeat(v, n):
    out = []
    i = 0
    while i < n:
        out.append(v)
        i = i + 1
    return out

def chunk(xs, n):
    if n <= 0:
        raise "chunk() needs a positive size"
    out = []
    i = 0
    while i < len(xs):
        out.append(slice(xs, i, i + n))
        i = i + n
    return out
