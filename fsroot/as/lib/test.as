# aether: 3.0
# Assertions record failures and return bool; they do not throw. report()
# prints the counters and returns the failure count for a program's exit code.
# Module counters are ordinary typed globals, with explicit rebinding.
_passed: i64 = 0
_failed: i64 = 0


def _record(ok: bool) -> bool:
    global _passed, _failed
    if ok:
        _passed += 1
    else:
        _failed += 1
    return ok


def assert(cond: bool, msg: str) -> bool:
    if not cond:
        print("FAIL:", msg)
    return _record(cond)


def assert_eq[T: Equatable](got: T, want: T, msg: str) -> bool:
    ok = got == want
    if not ok:
        print("FAIL:", msg, "-- got", got, "want", want)
    return _record(ok)


def assert_ne[T: Equatable](got: T, want: T, msg: str) -> bool:
    ok = got != want
    if not ok:
        print("FAIL:", msg, "-- both", got)
    return _record(ok)


def assert_true(cond: bool, msg: str) -> bool:
    return assert(cond, msg)


def assert_false(cond: bool, msg: str) -> bool:
    return assert(not cond, msg)


def same[T: Equatable](a: List[T], b: List[T]) -> bool:
    if len(a) != len(b):
        return false
    index = 0
    while index < len(a):
        if a[index] != b[index]:
            return false
        index += 1
    return true


def assert_same[T: Equatable](got: List[T], want: List[T], msg: str) -> bool:
    return assert(same(got, want), msg)


def fail(msg: str) -> bool:
    return assert(false, msg)


def reset() -> None:
    global _passed, _failed
    _passed = 0
    _failed = 0


def passed() -> i64:
    return _passed


def failed() -> i64:
    return _failed


def summary() -> Dict[str, i64]:
    return {"passed": _passed, "failed": _failed}


def report() -> i64:
    print("tests:", _passed, "passed,", _failed, "failed")
    return _failed
