# aether: 3.0
# Keep the Buffer owner live. Raw pointers deliberately carry no GC root;
# safe Region views will be a separate ownership-checked interface.
def read[T: Integer](pointer: Ptr[T], index: i64) -> T:
    unsafe:
        return pointer[index]


def write[T: Integer](pointer: Ptr[T], index: i64, value: T) -> None:
    unsafe:
        pointer[index] = value


def main() -> None:
    owner = buffer(64)
    unsafe:
        base = addr(owner)
        byte = i8ptr(base)
        short = i16ptr(base + u64(1))
        word = i32ptr(base + u64(5))
        wide = i64ptr(base + u64(13))
        byte[0] = -128
        short[0] = -32768
        word[0] = -2147483648
        wide[0] = -9223372036854775808
        assert read(byte, 0) == -128
        assert read(short, 0) == -32768
        assert read(word, 0) == -2147483648
        assert read(wide, 0) == -9223372036854775808
        assert i64(read(byte, 0)) == -128
        assert i64(read(short, 0)) == -32768
        assert i64(read(word, 0)) == -2147483648
        assert addr(word) == base + u64(5)
        # Typed writes use the declared width, including unaligned addresses.
        assert owner[0] == 128 and owner[2] == 128
        assert owner[8] == 128 and owner[20] == 128
        word[1] = 1337
        word[1] += 5
        assert word[1] == 1342
        write(word, 1, i32(-7))
        middle = i32ptr(base + u64(9))
        assert middle[-1] == -2147483648 and middle[0] == -7
        pointers: List[Ptr[i32]] = [word, middle]
        assert pointers[0] == word and pointers[0] != pointers[1]
        boxed = Any(word)
        assert cast[Ptr[i32]](boxed) == word
        assert str(i32ptr(0)) == "Ptr[i32](0x0)"
        assert str([i32ptr(0)]) == "[Ptr[i32](0x0)]"
        optional: Optional[Ptr[i32]] = word
        if optional is not None:
            assert optional[1] == -7
        byte[32] = 65
        byte[33] = 66
        byte[34] = 0
        text = i8ptr(base + u64(32))
        assert mem2str(text, 2) == "AB" and mem2cstr(text, 3) == "AB"
        failures = 0
        try:
            i8ptr(0)[0]
        except ValueError:
            failures += 1
        try:
            i64ptr(base)[9223372036854775807]
        except OverflowError:
            failures += 1
        try:
            i8ptr(18446744073709551615)[1]
        except OverflowError:
            failures += 1
        try:
            i8ptr(1)[-2]
        except OverflowError:
            failures += 1
        try:
            i8ptr(1)[-1]
        except ValueError:
            failures += 1
        assert failures == 5
        gc_collect()
        assert word[1] == -7
        assert len(owner) == 64
    print("native typed pointers ok")
