# aether: 3.0
# Raw addresses here always refer to this program's own live memory. Managed
# Buffer/Bytes arguments carry bounds; conversion returns an independent str.
def copied() -> str:
    unsafe:
        return mem2str(Bytes("临时文本"), 12)

def main() -> None:
    data = buffer(8)
    initial = Bytes("hello")
    for index in range(len(initial)):
        data[index] = initial[index]
    unsafe:
        first = mem2str(data, 5)
        second = mem2cstr(data)
        assert first == "hello" and second == first
        assert mem2str(addr(data), 5) == first
        assert mem2cstr(addr(data), 8) == first
        assert mem2str(data, 0) == ""
        assert mem2str(Bytes("中文"), 6) == "中文"
        assert mem2str(Bytes("a" + chr(0) + "b"), 3) == "a" + chr(0) + "b"
        assert peek8(addr(Bytes("A"))) == 65
        data[0] = 88
        gc_collect()
        assert first == "hello" and second == "hello"
        saved = copied()
        gc_collect()
        assert saved == "临时文本"

        errors = 0
        try:
            mem2str(data, 9)
        except IndexError:
            errors += 1
        try:
            mem2str(data, -1)
        except ValueError:
            errors += 1
        try:
            mem2str(data, 67108865)
        except ValueError:
            errors += 1
        try:
            null: u64 = 0
            mem2str(null, 0)
        except ValueError:
            errors += 1
        try:
            mem2cstr(Bytes("no terminator"))
        except ValueError:
            errors += 1
        try:
            mem2cstr(data, 0)
        except ValueError:
            errors += 1
        invalid = buffer(1)
        invalid[0] = 255
        try:
            mem2str(invalid, 1)
        except ConversionError as error:
            assert error.line > 0 and error.column > 0
            errors += 1
        assert errors == 7
    print("native memory text ok")
