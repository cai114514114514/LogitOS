# aether: 3.0
Pair = layout("pair", 2, [["first", 0, 1, "u"], ["second", 1, 1, "u"]])

def copy[B: ByteStorage](data: B) -> Bytes:
    assert len(data) == 2
    assert 65 in data
    first, second = data
    assert first == 65 and second == 66
    values = [byte for byte in data]
    total = 0
    for byte in data:
        total += byte
    assert total == 131 and values[1] == 66
    unsafe:
        assert peek8(addr(data)) == 65
        assert mem2str(data, 2) == "AB"
    return Bytes(data)

def fill[B: MutableByteStorage](data: B) -> Bytes:
    data[0] = 64
    data[0] += 1
    data[1] = 66
    # A mutable promise may be passed to a read-only generic function.
    return copy(data)

def main() -> None:
    assert fill(buffer(2)) == Bytes("AB")
    assert fill(Pair()) == Bytes("AB")
    assert copy(Bytes("AB")) == Bytes("AB")
    print("native storage protocols ok")
