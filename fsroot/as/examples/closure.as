# aether: 3.0
# Captures retain shared bindings after the defining function has returned.
# The counter's integer stays in one native cell shared with its closure.

def counter() -> Callable[[], i64]:
    count = 0

    def increment() -> i64:
        count += 1
        return count

    return increment


def adder(amount: i64) -> Callable[[i64], i64]:
    return lambda value: value + amount


def main() -> None:
    function = counter()
    print("counts:", function(), function(), function())
    double: Callable[[i64], i64] = lambda value: value * 2
    print("double 21:", double(21))
    print("add10 to 5:", adder(10)(5))
