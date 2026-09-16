# aether: 3.0
# Descriptive statistics over native homogeneous lists. Convert each integer
# before real-valued accumulation, so two valid i64 inputs cannot overflow an
# unintended integer intermediate. Sorting copies its input.
import math
import seq


def sum[T: Number](xs: List[T]) -> T:
    return math.sum(xs)


def count[T](xs: List[T]) -> i64:
    return len(xs)


def min[T: Number](xs: List[T]) -> T:
    if len(xs) == 0:
        raise ValueError("min() needs a non-empty list")
    result = xs[0]
    for value in xs:
        if value < result:
            result = value
    return result


def max[T: Number](xs: List[T]) -> T:
    if len(xs) == 0:
        raise ValueError("max() needs a non-empty list")
    result = xs[0]
    for value in xs:
        if value > result:
            result = value
    return result


def range[T: Number](xs: List[T]) -> T:
    return max(xs) - min(xs)


def mean[T: Number](xs: List[T]) -> f64:
    return math.mean(xs)


def median[T: Number](xs: List[T]) -> Any:
    if len(xs) == 0:
        raise ValueError("median() needs a non-empty list")
    values = seq.sorted(xs)
    middle = len(values) / 2
    # The old API preserves T for odd counts and returns f64 for even counts.
    # Any makes that value-dependent result explicit without rounding odd i64s.
    if len(values) % 2 == 1:
        return Any(values[middle])
    return Any((f64(values[middle - 1]) + f64(values[middle])) / 2.0)


def variance[T: Number](xs: List[T]) -> f64:
    if len(xs) == 0:
        raise ValueError("variance() needs a non-empty list")
    average = mean(xs)
    total = 0.0
    for value in xs:
        delta = f64(value) - average
        total += delta * delta
    return total / f64(len(xs))


def sample_variance[T: Number](xs: List[T]) -> f64:
    if len(xs) < 2:
        raise ValueError("sample_variance() needs at least two values")
    average = mean(xs)
    total = 0.0
    for value in xs:
        delta = f64(value) - average
        total += delta * delta
    return total / f64(len(xs) - 1)


def stddev[T: Number](xs: List[T]) -> f64:
    return math.sqrt(variance(xs))


def sample_stddev[T: Number](xs: List[T]) -> f64:
    return math.sqrt(sample_variance(xs))


def frequencies[K: Hashable](xs: List[K]) -> Dict[K, i64]:
    out: Dict[K, i64] = {}
    for value in xs:
        out[value] = out.get(value, 0) + 1
    return out


def zscores[T: Number](xs: List[T]) -> List[f64]:
    deviation = stddev(xs)
    average = mean(xs)
    out: List[f64] = []
    for value in xs:
        if deviation == 0.0:
            out.append(0.0)
        else:
            out.append((f64(value) - average) / deviation)
    return out


def moving_average[T: Number](xs: List[T], width: i64) -> List[f64]:
    if width <= 0:
        raise ValueError("moving_average() needs a positive width")
    out: List[f64] = []
    i = 0
    while i < len(xs):
        start = i - width + 1
        if start < 0:
            start = 0
        total = 0.0
        j = start
        while j <= i:
            total += f64(xs[j])
            j += 1
        out.append(total / f64(i - start + 1))
        i += 1
    return out
