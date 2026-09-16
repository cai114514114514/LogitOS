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
    print("中文 native", sum_squares(values), point.x)
    return 0
