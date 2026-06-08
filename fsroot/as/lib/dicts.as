# dicts -- helpers over dicts (a dict already has .get/.has/.keys/.values/.remove)
#   from dicts import items, merge

def items(d):                       # [[key, value], ...]
    out = []
    for k in d:                     # iterating a dict yields its keys
        out.append([k, d[k]])
    return out

def merge(a, b):                    # new dict: a then b (b wins on conflict)
    out = {}
    for k in a:
        out[k] = a[k]
    for k in b:
        out[k] = b[k]
    return out

def from_pairs(pairs):              # [[k, v], ...] -> dict
    out = {}
    for p in pairs:
        out[p[0]] = p[1]
    return out

def get(d, k, default):
    return d.get(k, default)
