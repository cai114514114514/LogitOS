# aether: 3.0
# Exact integers by default; wrapping is an explicit low-level operation.
def main() -> None:
    maximum = 9223372036854775807
    minimum = -9223372036854775808
    print(minimum)
    caught = false
    try:
        result = maximum + 1
    except OverflowError as error:
        # A3 errors are typed records with a source location, rather than the
        # old exception-binding string. Show the runtime's actual message.
        print(error.message)
        caught = true
    assert caught
    print(wrapping_add(maximum, 1))
    print(wrapping_shl(1, 63))
    print(parse_int("0x7fffffffffffffff"))
