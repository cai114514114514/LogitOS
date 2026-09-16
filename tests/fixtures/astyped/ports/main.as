# aether: 3.0
# Actual files and descriptor reuse prove that with owns cleanup. The same
# program runs on the host and under the guest's much smaller descriptor table.

def early(path: str) -> i64:
    with file = open(path, "r"):
        result = file.read_exact(2)
        return len(result)

def fail() -> None:
    raise ValueError("body failed")

def returned_text(path: str) -> str:
    with file = open(path):
        return file.readall().decode()

def main() -> None:
    path = args()[1] + "/ports.txt"
    with file = open(path, "w"):
        assert file.kind() == "file"
        assert not file.closed()
        assert file.write("first\r\n\n中文\nlast") == 19
    with file = open(path):
        first = file.line()
        assert first != None
        assert first == "first"
        rest: List[str] = []
        for line in file.lines():
            rest.append(line)
        assert len(rest) == 3
        assert rest[0] == ""
        assert rest[1] == "中文"
        assert rest[2] == "last"
        assert file.line() == None
        assert file.read(1) == None
        assert len(file.readall()) == 0

    with file = open(path, "rw"):
        assert file.read_exact(2) == Bytes("fi")
        # The runtime may have buffered past the logical cursor. Writing must
        # replace byte two, rather than append at the physical read-ahead offset.
        assert file.write(Bytes("X")) == 1
    assert file_read(path).decode() == "fiXst\r\n\n中文\nlast"

    # Every acquisition should reuse the same lowest available descriptor.
    # Merely checking output would miss a leaked fd until the process limit.
    descriptor = -1
    for index in range(96):
        with file = open(path):
            if descriptor == -1:
                descriptor = file.fd()
            assert file.fd() == descriptor
            if index % 2 == 0:
                continue
        assert early(path) == 2
    for index in range(3):
        with file = open(path):
            assert file.fd() == descriptor
            break

    caught = False
    try:
        with outer = open(path):
            with inner = open(path):
                fail()
    except ValueError as error:
        assert error.message == "body failed"
        caught = True
    assert caught
    with file = open(path):
        assert file.fd() == descriptor
        try:
            fail()
        except ValueError:
            # The handler is inside this with, so its owner must stay open.
            assert not file.closed()
            assert file.read_exact(2) == Bytes("fi")
        file.close()
        assert file.closed()
        assert file.fd() == -1
        file.close()
    with file = open(path, "a"):
        written: Optional[i64] = file.write("!")
        assert written != None
        assert written == 1
    assert file_read(path).decode() == "fiXst\r\n\n中文\nlast!"
    saved = returned_text(path)
    gc_collect()
    assert saved == "fiXst\r\n\n中文\nlast!"

    # A failed inner acquisition must release the already acquired outer fd.
    caught = False
    try:
        with outer = open(path):
            with missing = open(path + "/missing"):
                pass
    except IOError:
        caught = True
    assert caught
    with file = open(path):
        assert file.fd() == descriptor
        contents: Optional[Bytes] = file.readall()
        assert contents != None
        assert contents.decode() == saved

    # A name hidden by with becomes visible again after its owner is dropped.
    file = 41
    with file = open(path):
        assert file.fd() == descriptor
    assert file == 41
    print("native scoped ports ok")
