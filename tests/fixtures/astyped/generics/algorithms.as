# aether: 3
# Unconstrained T supports passing and returning values. Arithmetic needs a
# protocol, and T(0) makes a literal conversion explicit for each machine type.
def identity[T](value: T) -> T:
    return value

def total[T: Number](values: Slice[T]) -> T:
    result = T(0)
    for value in values:
        result += value
    return result

def twice[T: Number](value: T) -> T:
    return value + value

def composed[U: Number](value: U) -> U:
    return identity(twice(value))

def pair[T](left: T, right: T) -> Array[T, 2]:
    return [left, right]

def first[T](values: Slice[T]) -> T:
    return values[0]

def factorial[T: Integer](value: T) -> T:
    if value <= T(1):
        return T(1)
    return value * factorial(value - T(1))
