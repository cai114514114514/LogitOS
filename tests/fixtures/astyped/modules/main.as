# aether: 3
import math

def main() -> i64:
    total = 0
    for value in range(1, 5):
        total += math.square(value)
    print(total)
    return 0
