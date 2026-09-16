# aether: 3.0
# The harness supplies a private directory. The same binary exercises actual
# host files and the guest filesystem; no compiler or VM runs in the guest.
def collected_payload() -> Bytes:
    gc_collect()
    data = buffer(9001)
    for index in range(len(data)):
        data[index] = index % 256
    return Bytes(data)


def worker(directory: str, restricted: bool) -> None:
    authority = caps().bits()
    if (authority & CAP_FS_READ) == 0:
        denied = 0
        try:
            file_read(directory + "/fixture")
        except PermissionError:
            denied += 1
        try:
            file_write(directory + "/denied", Bytes("forbidden"))
        except PermissionError:
            denied += 1
        assert denied == 2
        return

    assert file_read(directory + "/fixture") == Bytes("native guest acceptance\n")
    assert file_read(directory + "/fixture").decode() == "native guest acceptance\n"
    path = directory + "/中文.bin"
    assert file_write(directory + "/中文.bin", collected_payload()) == 9001
    data = file_read(path)
    gc_collect()
    assert len(data) == 9001 and data[0] == 0 and data[255] == 255
    assert data[-1] == 40
    assert file_write(path, Bytes("short")) == 5
    assert file_read(path) == Bytes("short")
    assert file_write(path, Bytes("")) == 0 and len(file_read(path)) == 0

    errors = 0
    try:
        file_read(directory + "/missing")
    except IOError as error:
        assert error.line > 0 and error.column > 0
        assert "main.as" in error.file
        errors += 1
    try:
        file_write(directory + "/missing/child", Bytes("x"))
    except IOError:
        errors += 1
    try:
        file_read(directory + "/fixture" + chr(0) + "suffix")
    except ValueError:
        errors += 1
    try:
        file_read("")
    except ValueError:
        errors += 1
    assert errors == 4

    if restricted:
        denied = 0
        try:
            file_read(directory + "/../state-outside/fixture")
        except PermissionError:
            denied += 1
        try:
            file_write(directory + "-outside/fixture", Bytes("changed"))
        except PermissionError:
            denied += 1
        assert denied == 2


def main() -> None:
    arguments = args()
    if len(arguments) == 3:
        assert caps().bits() == parse_int(arguments[1])
    worker(arguments[-1], len(arguments) == 3)
    gc_collect()
    print("native files ok")
