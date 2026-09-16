# aether: 3.0

# A concrete instance must retain both the i8 overflow check and this source
# position after optimization. The caller must stop when the instance fails.
def twice[T: Integer](value: T) -> T:
    return value + value

def main() -> None:
    maximum: i8 = 127
    result = twice(maximum)
    print("UNREACHABLE", result)
