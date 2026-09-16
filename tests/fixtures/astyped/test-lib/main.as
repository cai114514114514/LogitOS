# aether: 3.0
import test
from test import assert


def exercise() -> None:
    test.reset()
    test.assert(true, "direct")
    assert(true, "imported")
    test.assert_eq(7, 7, "equal")
    test.assert_ne("a", "b", "different")
    test.assert_true(true, "true")
    test.assert_false(false, "false")
    test.assert_same([1, 2], [1, 2], "list values")
    assert test.passed() == 7 and test.failed() == 0
    assert test.same([1, 2], [1, 2])
    assert not test.same([1], [1, 2]) and not test.same([1], [2])
    assert (test.passed()) == 7
    test.assert(false, "recorded")
    test.assert_eq([1], [1], "list identity")
    test.assert_ne("x", "x", "same text")
    test.assert_same([1], [2], "different values")
    test.fail("explicit")
    assert test.failed() == 5
    state = test.summary()
    assert state["passed"] == 7 and state["failed"] == 5
    assert test.report() == 5
    test.reset()
    assert test.passed() == 0 and test.failed() == 0
    # summary is an owned snapshot, independent of future counter updates.
    assert state["passed"] == 7 and state["failed"] == 5
    assert test.report() == 0


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native test library ok")
