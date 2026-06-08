# test -- minimal assertions for AetherScript
#   from test import assert, assert_eq, report
# (functions can't reassign module globals, so the pass/fail counts live in a
#  mutable list that the functions mutate in place.)

_n = [0, 0]                         # [passed, failed]

def assert(cond, msg):
    if cond:
        _n[0] = _n[0] + 1
    else:
        _n[1] = _n[1] + 1
        print("FAIL:", msg)
    return cond

def assert_eq(got, want, msg):
    ok = got == want
    if ok:
        _n[0] = _n[0] + 1
    else:
        _n[1] = _n[1] + 1
        print("FAIL:", msg, "-- got", got, "want", want)
    return ok

def assert_ne(got, want, msg):
    ok = got != want
    if ok:
        _n[0] = _n[0] + 1
    else:
        _n[1] = _n[1] + 1
        print("FAIL:", msg, "-- both", got)
    return ok

def assert_true(cond, msg):
    return assert(cond, msg)

def assert_false(cond, msg):
    return assert(not cond, msg)

def same(a, b):                     # flat-list equality (elements compared with ==)
    if len(a) != len(b):            # (as `==` on lists is identity-based, not structural)
        return false
    i = 0
    ok = true
    while i < len(a) and ok:
        if a[i] != b[i]:
            ok = false
        i = i + 1
    return ok

def assert_same(got, want, msg):
    return assert(same(got, want), msg)

def fail(msg):
    return assert(false, msg)

def reset():
    _n[0] = 0
    _n[1] = 0
    return

def passed():
    return _n[0]

def failed():
    return _n[1]

def summary():
    return {"passed": _n[0], "failed": _n[1]}

def report():
    print("tests:", _n[0], "passed,", _n[1], "failed")
    return _n[1]
