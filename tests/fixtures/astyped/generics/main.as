# aether: 3
import algorithms

struct Point:
    x: i64

def main() -> None:
    integers: Array[i64, 3] = [1, 2, 3]
    floats: Array[f64, 2] = [1.25, 2.5]
    point = algorithms.identity(Point(7))
    copied = algorithms.identity(integers)
    copied[0] = 99
    assert integers[0] == 1
    assert point.x == 7
    assert algorithms.identity(True)
    assert algorithms.first(algorithms.pair(10, 20)) == 10
    print(algorithms.total(integers), algorithms.total(floats))
    print(algorithms.composed(4), algorithms.composed(1.5))
    print(algorithms.factorial(5), algorithms.identity("泛型"))
