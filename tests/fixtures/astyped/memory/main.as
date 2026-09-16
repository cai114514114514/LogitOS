# aether: 3.0
# Raw operations use this program's own live storage. Unsafe does not turn an
# arbitrary address into a checked slice, and grants no capability by itself.
def first_byte[T](unused: T) -> i64:
    unsafe:
        return peek8(addr("A"))

def closure_read() -> i64:
    unsafe:
        data = buffer(1)
        data[0] = 77
        def read() -> i64:
            unsafe:
                return peek8(addr(data))
        return read()

def main() -> None:
    data = buffer(16)
    data[15] = 43
    unsafe:
        address = addr(data)
        poke8(address, 384)
        assert peek8(address) == 128
        assert data[0] == 128
        poke16(address + 1, 4660)
        assert peek16(address + 1) == 4660
        poke32(address + 3, -1)
        assert peek32(address + 3) == 4294967295
        poke64(address + 7, -9223372036854775808)
        assert peek64(address + 7) == -9223372036854775808
        assert data[15] == 43
        assert data[1] == 52
        assert data[2] == 18
        poke8(address, 256)
        assert peek8(address) == 0
        poke16(address, -1)
        assert peek16(address) == 65535
        gc_collect()
        assert peek16(address) == 65535

        # Read a managed substring after the original text-producing call.
        text = ("a" + str(12345)).slice(1, 4)
        assert peek8(addr(text)) == ord("1")
        assert addr(buffer(0)) != 0
        try:
            peek8(0)
            assert False
        except ValueError:
            pass

    count = 0
    for index in range(5):
        unsafe:
            if index == 2:
                break
            count += 1
    assert count == 2
    assert closure_read() == 77
    assert first_byte(42) == 65
    assert first_byte("中文") == 65
    print("native raw memory ok")
