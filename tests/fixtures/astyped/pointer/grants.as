# aether: 3.0
# The guest runner supplies actual kernel grants. Pointer construction must
# be denied before even a null raw address can enter the typed program.
def main() -> None:
    expected = parse_int(args()[1])
    assert caps().bits() == expected
    if (expected & CAP_RAW) != 0:
        storage = buffer(8)
        unsafe:
            pointer = i32ptr(addr(storage))
            pointer[0] = 17
            pointer[1] = -7
            assert pointer[0] + pointer[1] == 10
    else:
        denied = 0
        unsafe:
            try:
                i8ptr(0)
            except PermissionError:
                denied += 1
            try:
                i16ptr(0)
            except PermissionError:
                denied += 1
            try:
                i32ptr(0)
            except PermissionError:
                denied += 1
            try:
                i64ptr(0)
            except PermissionError:
                denied += 1
        assert denied == 4
    print("native pointer grants ok")
