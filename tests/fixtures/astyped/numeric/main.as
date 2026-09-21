# aether: 3
struct Vec2:
    x: f64
    y: f64

def sum_squares(values: Slice[f64]) -> f64:
    total = 0.0
    for x in values:
        total += x * x
    return total

def main() -> i64:
    values: Array[f64, 4] = [1.0, 2.0, 3.0, 4.0]
    point = Vec2(2.0, 3.0)
    point.x = 4.0
    # Float power (llvm.pow, IEEE semantics -- see LANGUAGE_3.md section 3):
    # no overflow trap (out-of-range is inf), negative exponents are values,
    # domain errors are NaN. These distinguish it from checked_power, whose
    # ValueError/OverflowError edges the integer compound fixture guards.
    assert 2.0 ** 10.0 == 1024.0
    assert 2.0 ** -2.0 == 0.25
    assert 9.0 ** 0.5 == 3.0
    assert 0.0 ** 0.0 == 1.0
    squared: f32 = 2.0
    eighth: f32 = 8.0
    wide: f32 = 256.0
    assert squared ** eighth == wide
    # inf is a representable result, not an error; NaN never equals itself.
    assert 1e308 ** 2.0 > 1e307
    bad = (-1.0) ** 0.5
    assert not (bad == bad)
    print("中文 native", sum_squares(values), point.x)
    return 0
