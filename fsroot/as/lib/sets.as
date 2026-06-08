# sets -- small set helpers backed by dict keys
#   values must be valid dict keys: strings or ints.

def empty():
    return {}

def from_list(xs):
    out = {}
    for x in xs:
        out[x] = true
    return out

def to_list(s):
    return s.keys()

def size(s):
    return len(s)

def contains(s, value):
    return s.has(value)

def add(s, value):                  # mutates and returns s
    s[value] = true
    return s

def remove(s, value):               # mutates and returns whether it existed
    return s.remove(value)

def copy(s):
    out = {}
    for x in s:
        out[x] = true
    return out

def union(a, b):
    out = copy(a)
    for x in b:
        out[x] = true
    return out

def intersection(a, b):
    out = {}
    for x in a:
        if b.has(x):
            out[x] = true
    return out

def difference(a, b):
    out = {}
    for x in a:
        if not b.has(x):
            out[x] = true
    return out

def symmetric_difference(a, b):
    out = {}
    for x in a:
        if not b.has(x):
            out[x] = true
    for x in b:
        if not a.has(x):
            out[x] = true
    return out

def is_subset(a, b):
    for x in a:
        if not b.has(x):
            return false
    return true

def is_superset(a, b):
    return is_subset(b, a)

def disjoint(a, b):
    for x in a:
        if b.has(x):
            return false
    return true

def equal(a, b):
    return len(a) == len(b) and is_subset(a, b)
