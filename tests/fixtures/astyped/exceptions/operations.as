# aether: 3.0

def divide(left: i64, right: i64) -> i64:
    return left / right

def twice[T: Integer](value: T) -> T:
    return value + value

def fail() -> None:
    raise ValueError("中文错误")

def recover() -> i64:
    try:
        return divide(1, 0)
    except ZeroDivisionError:
        return 42
