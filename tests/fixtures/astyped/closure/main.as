# aether: 3.0
# Native closure contract. Each captured binding is shared by all closures
# created from that invocation; separate invocations own separate bindings.
from seq import map


def counter() -> Callable[[], i64]:
    count = 0

    def increment() -> i64:
        count += 1
        return count

    return increment


def adder(amount: i64) -> Callable[[i64], i64]:
    return lambda value: value + amount


def shared() -> List[Callable[[], i64]]:
    count = 0

    def increment() -> i64:
        count += 1
        return count

    def read() -> i64:
        return count

    return [increment, read]


def keep[T](value: T) -> Callable[[], T]:
    return lambda: value


def nested() -> Callable[[], str]:
    text = "transitive " + "capture"

    def middle() -> Callable[[], str]:
        return lambda: text

    return middle()


def recursive() -> Callable[[i64], i64]:
    def factorial(value: i64) -> i64:
        if value == 0:
            return 1
        return value * factorial(value - 1)

    return factorial


def reassign() -> Callable[[], str]:
    text = "before"
    get: Callable[[], str] = lambda: text
    text = "after " + "assignment"
    return get


def failing() -> Callable[[], i64]:
    message = "closure " + "failure"

    def fail() -> i64:
        gc_collect()
        raise ValueError(message)

    return fail


def branched() -> Callable[[bool], i64]:
    value = 7

    def choose(first: bool) -> i64:
        if first:
            return value
        return value + 1

    return choose


class Holder:
    value: i64

    def reader(self) -> Callable[[], i64]:
        return lambda: self.value


def conditional_capture() -> Callable[[bool], i64]:
    value = 9

    def choose(first: bool) -> i64:
        # First-branch checking discovers the capture. Restoring flow facts
        # must not read a compiler stack slot which has never been initialized.
        return value if first else value + 1

    return choose


def worker() -> None:
    choose = conditional_capture()
    assert choose(true) == 9 and choose(false) == 10
    first = counter()
    second = counter()
    assert first() == 1 and first() == 2
    assert second() == 1 and first() == 3
    pair = shared()
    gc_collect()
    assert pair[0]() == 1 and pair[1]() == 1
    assert pair[0]() == 2 and pair[1]() == 2
    assert adder(10)(5) == 15
    assert (lambda value: value * 2)(21) == 42
    double: Callable[[i64], i64] = lambda value: value * 2
    assert double(21) == 42
    squared = map(lambda value: value * value, [1, 2, 3])
    assert squared[2] == 9
    integer = keep(42)
    text = keep("generic " + "capture")
    transitive = nested()
    reassigned = reassign()
    factorial = recursive()
    gc_collect()
    assert integer() == 42 and text() == "generic capture"
    assert transitive() == "transitive capture"
    assert reassigned() == "after assignment"
    assert factorial(5) == 120
    choose = branched()
    assert choose(true) == 7 and choose(false) == 8
    late: List[Callable[[], i64]] = [lambda: index for index in range(3)]
    gc_collect()
    assert late[0]() == 2 and late[2]() == 2
    holder = Holder(7)
    reader = holder.reader()
    holder.value = 9
    gc_collect()
    assert reader() == 9
    nothing: Callable[[], None] = lambda: None
    nothing()
    optional: Callable[[], Optional[i64]] = lambda: None
    assert optional() == None
    dynamic = Any(first)
    gc_collect()
    assert cast[Callable[[], i64]](dynamic)() == 4
    try:
        failing()()
        assert false
    except ValueError as error:
        assert error.message == "closure failure" and error.line > 0


def main() -> None:
    worker()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native closures ok")
