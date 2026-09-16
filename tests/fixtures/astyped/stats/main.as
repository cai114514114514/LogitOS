# aether: 3.0
import stats


def close(value: f64, expected: f64) -> None:
    assert value > expected - 0.000001 and value < expected + 0.000001


def exercise() -> None:
    values = [4, 1, 3, 2]
    assert stats.sum(values) == 10 and stats.count(values) == 4
    assert stats.min(values) == 1 and stats.max(values) == 4
    assert stats.range(values) == 3 and stats.mean(values) == 2.5
    assert cast[f64](stats.median(values)) == 2.5
    assert values[0] == 4 and values[1] == 1
    assert cast[i64](stats.median([7, 1, 3])) == 3
    assert cast[f64](stats.median([2.5, 1.5, 3.5])) == 2.5
    close(stats.variance(values), 1.25)
    close(stats.sample_variance(values), 1.6666666666666667)
    close(stats.stddev(values), 1.118033988749895)
    close(stats.sample_stddev(values), 1.2909944487358056)
    scores = stats.zscores([1.0, 2.0, 3.0])
    close(scores[0], -1.224744871391589)
    close(scores[1], 0.0)
    close(scores[2], 1.224744871391589)
    zeros = stats.zscores([3, 3, 3])
    assert zeros[0] == 0.0 and zeros[2] == 0.0
    moving = stats.moving_average([1, 2, 3, 4], 2)
    assert moving[0] == 1.0 and moving[1] == 1.5
    assert moving[2] == 2.5 and moving[3] == 3.5
    all_values = stats.moving_average([1, 2, 3], 9223372036854775807)
    assert all_values[2] == 2.0
    limit = 9223372036854775807
    assert cast[i64](stats.median([limit])) == limit
    assert cast[f64](stats.median([limit, limit])) == f64(limit)
    assert stats.mean([limit, limit]) == f64(limit)
    assert stats.moving_average([limit, limit], 2)[1] == f64(limit)
    frequency = stats.frequencies(["a " + "word", "b", "a word"])
    gc_collect()
    assert frequency["a word"] == 2 and frequency["b"] == 1
    assert stats.frequencies([1, 2, 1])[1] == 2
    empty: List[i64] = []
    assert stats.sum(empty) == 0 and stats.count(empty) == 0
    assert len(stats.frequencies(empty)) == 0
    assert len(stats.moving_average(empty, 1)) == 0
    assert stats.sum([i8(1), i8(2)]) == i8(3)
    assert stats.range([u64(1), u64(10)]) == u64(9)
    caught = 0
    try:
        stats.min(empty)
    except ValueError:
        caught += 1
    try:
        stats.max(empty)
    except ValueError:
        caught += 1
    try:
        stats.mean(empty)
    except ValueError:
        caught += 1
    try:
        stats.median(empty)
    except ValueError:
        caught += 1
    try:
        stats.variance(empty)
    except ValueError:
        caught += 1
    try:
        stats.sample_variance([1])
    except ValueError:
        caught += 1
    try:
        stats.moving_average(values, 0)
    except ValueError:
        caught += 1
    try:
        stats.sum([limit, 1])
    except OverflowError:
        caught += 1
    assert caught == 8


def main() -> None:
    exercise()
    gc_collect()
    assert gc_live_bytes() == 0
    print("native stats library ok")
