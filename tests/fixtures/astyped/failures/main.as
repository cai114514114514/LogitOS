# aether: 3
def overflow() -> i64:
    value: i8 = 127
    value += 1
    return i64(value)

def main() -> None:
    result = overflow()
    print("UNREACHABLE", result)
