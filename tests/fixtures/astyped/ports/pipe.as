# aether: 3.0
# Write only a short payload before reading: an anonymous pipe is bounded,
# so larger producers must run concurrently with their consumer.
def early(expected: i64) -> Bytes:
    with reader, writer = pipe():
        assert reader.fd() == expected
        writer.write("retained")
        writer.close()
        return reader.readall()

def main() -> None:
    first = -1
    second = -1
    with reader, writer = pipe():
        first = reader.fd()
        second = writer.fd()
        assert reader.kind() == "pipe" and writer.kind() == "pipe"
        assert not reader.closed() and not writer.closed()
        assert writer.write("first\n中文\n") == 13
        writer.close()
        assert writer.closed() and writer.fd() == -1
        writer.close()
        assert reader.line() == "first"
        assert reader.line() == "中文"
        assert reader.line() == None
        assert reader.read(1) == None
        assert reader.readall() == Bytes("")

    # Both descriptors must be reused, including continue, break and return.
    # Checking only output would let one leaked endpoint pass unnoticed.
    for index in range(96):
        with reader, writer = pipe():
            assert reader.fd() == first and writer.fd() == second
            if index % 2 == 0:
                continue
    for index in range(3):
        with reader, writer = pipe():
            assert reader.fd() == first and writer.fd() == second
            break
    saved = early(first)
    gc_collect()
    assert saved == Bytes("retained")

    caught = False
    try:
        with reader, writer = pipe():
            with inner_reader, inner_writer = pipe():
                raise ValueError("release all four endpoints")
    except ValueError as error:
        assert error.message == "release all four endpoints"
        caught = True
    assert caught
    with reader, writer = pipe():
        assert reader.fd() == first and writer.fd() == second
        # A handler inside the owner scope keeps both endpoints available.
        failures = 0
        try:
            reader.write("wrong direction")
        except IOError:
            failures += 1
        try:
            writer.read_exact(1)
        except IOError:
            failures += 1
        assert failures == 2
        writer.write("still open")
        writer.close()
        assert reader.readall().decode() == "still open"

    reader = 41
    writer = 42
    caught = False
    try:
        with reader, writer = pipe():
            reader.close()
            writer.write("no reader")
    except IOError:
        caught = True
    assert caught
    with reader, writer = pipe():
        assert reader.fd() == first and writer.fd() == second
    assert reader == 41 and writer == 42
    print("native pipe owners ok")
