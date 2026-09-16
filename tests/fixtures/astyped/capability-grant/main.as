# aether: 3.0
# One native source is launched with different real kernel grants. Expected
# bits are independent arguments from the host ABI oracle, never from caps().
def read(address: usize, width: i64) -> i64:
    unsafe:
        if width == 8:
            return peek8(address)
        if width == 16:
            return peek16(address)
        if width == 32:
            return peek32(address)
        return peek64(address)

def write(address: usize, width: i64) -> None:
    unsafe:
        if width == 8:
            poke8(address, 127)
        elif width == 16:
            poke16(address, 127)
        elif width == 32:
            poke32(address, 127)
        else:
            poke64(address, 127)

def check_raw(expected: i64) -> None:
    allowed = (expected & CAP_RAW) != 0
    address: usize = 0
    data = buffer(8)
    denied = 0
    try:
        unsafe:
            address = addr(data)
        assert allowed
    except PermissionError:
        assert not allowed
        denied += 1

    for width in [8, 16, 32, 64]:
        try:
            write(address, width)
            assert allowed
        except PermissionError:
            assert not allowed
            denied += 1
        try:
            assert read(address, width) == 127
            assert allowed
        except PermissionError:
            assert not allowed
            denied += 1
    assert denied == (0 if allowed else 9)

def main() -> None:
    arguments = args()
    expected = parse_int(arguments[1])
    expected_path = arguments[2]
    grant = caps()
    assert grant.bits() == expected
    if expected_path == "":
        assert grant.path() == None
    else:
        assert grant.path() == expected_path
    assert grant.without(-1).bits() == 0
    assert caps().bits() == expected
    check_raw(expected)
    print("native kernel grant ok")
