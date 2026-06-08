# dicts -- helpers over dicts (a dict already has .get/.has/.keys/.values/.remove)
#   from dicts import items, merge, pick, frequencies

def items(d):                       # [[key, value], ...]
    out = []
    for k in d:                     # iterating a dict yields its keys
        out.append([k, d[k]])
    return out

def keys(d):
    return d.keys()

def values(d):
    return d.values()

def has(d, k):
    return d.has(k)

def get(d, k, default):
    return d.get(k, default)

def copy(d):
    out = {}
    for k in d:
        out[k] = d[k]
    return out

def merge(a, b):                    # new dict: a then b (b wins on conflict)
    out = copy(a)
    for k in b:
        out[k] = b[k]
    return out

def update(target, patch):           # mutates and returns target
    for k in patch:
        target[k] = patch[k]
    return target

def set_default(d, k, value):
    if not d.has(k):
        d[k] = value
    return d[k]

def pop(d, k, default):
    if d.has(k):
        v = d[k]
        d.remove(k)
        return v
    return default

def clear(d):
    ks = d.keys()
    for k in ks:
        d.remove(k)
    return d

def from_pairs(pairs):              # [[k, v], ...] -> dict
    out = {}
    for p in pairs:
        out[p[0]] = p[1]
    return out

def invert(d):                       # values must be valid dict keys
    out = {}
    for k in d:
        out[d[k]] = k
    return out

def pick(d, ks):
    out = {}
    for k in ks:
        if d.has(k):
            out[k] = d[k]
    return out

def omit(d, ks):
    out = {}
    for k in d:
        if not (k in ks):
            out[k] = d[k]
    return out

def without(d, k):
    out = copy(d)
    out.remove(k)
    return out

def equal(a, b):                     # shallow value equality
    if len(a) != len(b):
        return false
    for k in a:
        if not b.has(k):
            return false
        if a[k] != b[k]:
            return false
    return true

def frequencies(xs):
    out = {}
    for x in xs:
        out[x] = out.get(x, 0) + 1
    return out

def group_by(f, xs):
    out = {}
    for x in xs:
        k = f(x)
        bucket = out.get(k, nil)
        if bucket == nil:
            bucket = []
            out[k] = bucket
        bucket.append(x)
    return out
