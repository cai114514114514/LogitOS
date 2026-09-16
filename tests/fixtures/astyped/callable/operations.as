# aether: 3.0
def increment(value: i64) -> i64:
    return value + 1


callback: Callable[[i64], i64] = increment
