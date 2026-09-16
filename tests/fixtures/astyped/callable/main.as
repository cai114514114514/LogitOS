# aether: 3.0
import operations

struct Action:
    execute: Callable[[i64], i64]

calls: i64 = 0


def twice(value: i64) -> i64:
    return value * 2


def identity[T](value: T) -> T:
    return value


def map[T, R](callback: Callable[[T], R], values: List[T]) -> List[R]:
    result: List[R] = []
    for value in values:
        result.append(callback(value))
    return result


def apply[T, R](callback: Callable[[T], R], value: T) -> R:
    return callback(value)


def relay[T](value: T) -> T:
    return apply(identity, value)


def choose(flag: bool) -> Callable[[i64], i64]:
    if flag:
        return twice
    return operations.increment


def typed_identity() -> Callable[[str], str]:
    return identity


def total(values: Slice[i64]) -> i64:
    sum = 0
    for value in values:
        sum += value
    return sum


def collect(value: str) -> str:
    gc_collect()
    return value + "!"


def record() -> None:
    global calls
    calls += 1


def fail() -> i64:
    raise ValueError("callback failure")


def exercise() -> None:
    function = twice
    assert function(7) == 14
    function = operations.increment
    assert function(7) == 8
    assert operations.callback(8) == 9
    action = Action(twice)
    assert action.execute(6) == 12
    action.execute = operations.increment
    assert action.execute(6) == 7
    functions = [twice, operations.increment]
    gc_collect()
    assert functions[0](9) == 18
    assert functions[1](9) == 10
    assert choose(true)(4) == 8 and choose(false)(4) == 5
    assert apply(twice, 8) == 16
    assert relay(9) == 9 and relay("generic") == "generic"
    assert typed_identity()("signature") == "signature"
    mapped = map(identity, [1, 2, 3])
    assert mapped[2] == 3
    words = map(collect, ["first " + "word", "second"])
    assert words[0] == "first word!" and words[1] == "second!"
    integers: Callable[[i8], i8] = identity
    assert integers(i8(7)) == i8(7)
    borrow = total
    values: Array[i64, 3] = [1, 2, 3]
    assert borrow(values) == 6
    boxed = Any(twice)
    gc_collect()
    assert cast[Callable[[i64], i64]](boxed)(3) == 6
    void_callback = record
    void_callback()
    assert calls == 1
    failing = fail
    caught = false
    try:
        failing()
        record()
    except ValueError as error:
        assert error.message == "callback failure" and error.line > 0
        caught = true
    assert caught and calls == 1


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native callable ok")
