# aether: 3.0
# Native homogeneous list helpers. Copying operations preserve element values
# and never reorder their input. Callbacks use checked native signatures.
import math


def copy[T](xs: List[T]) -> List[T]:
    out: List[T] = []
    for value in xs:
        out.append(value)
    return out


def is_empty[T](xs: List[T]) -> bool:
    return len(xs) == 0


def first[T](xs: List[T]) -> T:
    if len(xs) == 0:
        raise ValueError("first() needs a non-empty list")
    return xs[0]


def last[T](xs: List[T]) -> T:
    if len(xs) == 0:
        raise ValueError("last() needs a non-empty list")
    return xs[len(xs) - 1]


def map[T, R](f: Callable[[T], R], xs: List[T]) -> List[R]:
    out: List[R] = []
    for value in xs:
        out.append(f(value))
    return out


def filter[T](f: Callable[[T], bool], xs: List[T]) -> List[T]:
    out: List[T] = []
    for value in xs:
        if f(value):
            out.append(value)
    return out


def reject[T](f: Callable[[T], bool], xs: List[T]) -> List[T]:
    out: List[T] = []
    for value in xs:
        if not f(value):
            out.append(value)
    return out


def partition[T](f: Callable[[T], bool], xs: List[T]) -> List[List[T]]:
    yes: List[T] = []
    no: List[T] = []
    for value in xs:
        if f(value):
            yes.append(value)
        else:
            no.append(value)
    return [yes, no]


def reduce[T, R](f: Callable[[R, T], R], xs: List[T], init: R) -> R:
    result = init
    for value in xs:
        result = f(result, value)
    return result


def foreach[T, R](f: Callable[[T], R], xs: List[T]) -> None:
    # R may be None. A callback result is intentionally discarded, so both
    # side-effect-only functions and functions returning values are accepted.
    for value in xs:
        f(value)


def concat[T](a: List[T], b: List[T]) -> List[T]:
    out = copy(a)
    for value in b:
        out.append(value)
    return out


def flatten1[T](xss: List[List[T]]) -> List[T]:
    out: List[T] = []
    for xs in xss:
        for value in xs:
            out.append(value)
    return out


def sum[T: Number](xs: List[T]) -> T:
    return math.sum(xs)


def maxl[T: Ordered](xs: List[T]) -> T:
    result = xs[0]
    for value in xs:
        if value > result:
            result = value
    return result


def minl[T: Ordered](xs: List[T]) -> T:
    result = xs[0]
    for value in xs:
        if value < result:
            result = value
    return result


def any[T](f: Callable[[T], bool], xs: List[T]) -> bool:
    for value in xs:
        if f(value):
            return true
    return false


def all[T](f: Callable[[T], bool], xs: List[T]) -> bool:
    for value in xs:
        if not f(value):
            return false
    return true


def find[T](f: Callable[[T], bool], xs: List[T]) -> i64:
    index = 0
    for value in xs:
        if f(value):
            return index
        index += 1
    return -1


def index_of[T: Equatable](xs: List[T], value: T) -> i64:
    index = 0
    for item in xs:
        if item == value:
            return index
        index += 1
    return -1


def last_index_of[T: Equatable](xs: List[T], value: T) -> i64:
    index = len(xs) - 1
    while index >= 0:
        if xs[index] == value:
            return index
        index -= 1
    return -1


def count[T: Equatable](xs: List[T], value: T) -> i64:
    result = 0
    for item in xs:
        if item == value:
            result += 1
    return result


def count_if[T](f: Callable[[T], bool], xs: List[T]) -> i64:
    result = 0
    for value in xs:
        if f(value):
            result += 1
    return result


def contains[T: Equatable](xs: List[T], value: T) -> bool:
    return value in xs


def unique[T: Equatable](xs: List[T]) -> List[T]:
    out: List[T] = []
    for value in xs:
        if not (value in out):
            out.append(value)
    return out


def reverse[T](xs: List[T]) -> List[T]:
    out: List[T] = []
    index = len(xs) - 1
    while index >= 0:
        out.append(xs[index])
        index -= 1
    return out


def sorted_by[T](xs: List[T], less: Callable[[T, T], bool]) -> List[T]:
    # Stable insertion sort preserves the original algorithm and tie order.
    out = copy(xs)
    index = 1
    while index < len(out):
        key = out[index]
        previous = index - 1
        while previous >= 0 and less(key, out[previous]):
            out[previous + 1] = out[previous]
            previous -= 1
        out[previous + 1] = key
        index += 1
    return out


def _asc[T: Ordered](a: T, b: T) -> bool:
    return a < b


def sorted[T: Ordered](xs: List[T]) -> List[T]:
    return sorted_by(xs, _asc)


def zip[A, B](a: List[A], b: List[B]) -> List[List[Any]]:
    # The existing API returns two-element lists, not tuples. Each cell opts
    # into Any explicitly because the two input element types can differ.
    count = len(a)
    if len(b) < count:
        count = len(b)
    out: List[List[Any]] = []
    index = 0
    while index < count:
        out.append([Any(a[index]), Any(b[index])])
        index += 1
    return out


def enumerate[T](xs: List[T]) -> List[List[Any]]:
    out: List[List[Any]] = []
    index = 0
    for value in xs:
        out.append([Any(index), Any(value)])
        index += 1
    return out


def take[T](xs: List[T], n: i64) -> List[T]:
    out: List[T] = []
    index = 0
    while index < n and index < len(xs):
        out.append(xs[index])
        index += 1
    return out


def drop[T](xs: List[T], n: i64) -> List[T]:
    out: List[T] = []
    index = n
    if index < 0:
        index = 0
    while index < len(xs):
        out.append(xs[index])
        index += 1
    return out


def slice[T](xs: List[T], start: i64, stop: i64) -> List[T]:
    out: List[T] = []
    index = start
    if index < 0:
        index = len(xs) + index
    if stop < 0:
        stop = len(xs) + stop
    if index < 0:
        index = 0
    if stop > len(xs):
        stop = len(xs)
    while index < stop:
        out.append(xs[index])
        index += 1
    return out


def repeat[T](value: T, n: i64) -> List[T]:
    out: List[T] = []
    index = 0
    while index < n:
        out.append(value)
        index += 1
    return out


def chunk[T](xs: List[T], n: i64) -> List[List[T]]:
    if n <= 0:
        raise ValueError("chunk() needs a positive size")
    out: List[List[T]] = []
    index = 0
    while index < len(xs):
        out.append(slice(xs, index, index + n))
        index += n
    return out
