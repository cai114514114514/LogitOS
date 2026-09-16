# aether: 3.0
# The private test runtime revokes held rights at the explicit GC call. Keep
# the buffer alive so even a deliberately disabled guard touches owned memory.
def main() -> None:
    data = buffer(8)
    unsafe:
        address = addr(data)
        gc_collect()
        denied = 0
        try:
            addr(data)
        except PermissionError:
            denied += 1
        try:
            peek8(address)
        except PermissionError:
            denied += 1
        try:
            peek16(address)
        except PermissionError:
            denied += 1
        try:
            peek32(address)
        except PermissionError:
            denied += 1
        try:
            peek64(address)
        except PermissionError:
            denied += 1
        try:
            poke8(address, 1)
        except PermissionError:
            denied += 1
        try:
            poke16(address, 2)
        except PermissionError:
            denied += 1
        try:
            poke32(address, 3)
        except PermissionError:
            denied += 1
        try:
            poke64(address, 4)
        except PermissionError:
            denied += 1
        assert denied == 9
    assert data[0] == 0
    print("native raw revocation ok")
