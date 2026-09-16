# aether: 3.0
# Each guest invocation receives an actual kernel grant from native-capture.
# The runtime cannot manufacture these bits from source-language values.
def main() -> None:
    arguments = args()
    authority = caps().bits()
    assert authority == parse_int(arguments[1])
    directory = arguments[2]
    allowed_pipe = (authority & CAP_PROC) != 0
    denied_pipe = False
    try:
        with reader, writer = pipe():
            assert allowed_pipe
            writer.write("grant")
            writer.close()
            assert reader.readall() == Bytes("grant")
    except PermissionError:
        denied_pipe = True
    assert denied_pipe == (not allowed_pipe)
    with console = port(1):
        message = "native borrowed stdio\n"
        assert console.write(message) == len(message)
    try:
        with absent = port(65535):
            # A raw grant permits naming a descriptor; it cannot make an
            # absent descriptor usable. The real write must still fail.
            assert (authority & CAP_RAW) != 0
            failed = false
            try:
                absent.write("x")
            except IOError:
                failed = true
            assert failed
    except PermissionError:
        assert (authority & CAP_RAW) == 0
    for mode in ["r", "w", "rw"]:
        required = CAP_FS_READ
        path = directory + "/fixture"
        if mode == "w":
            required = CAP_FS_WRITE
            path = directory + "/port-grant-output"
        elif mode == "rw":
            required = CAP_FS_READ | CAP_FS_WRITE
        allowed = (authority & required) == required
        denied = False
        try:
            with file = open(path, mode):
                assert allowed
                if mode == "w":
                    assert file.write("granted") == 7
                else:
                    assert file.readall().decode() == "native guest acceptance\n"
        except PermissionError:
            assert not allowed
            denied = True
        assert denied == (not allowed)

    scope_denials = 0
    try:
        with file = open(directory + "/../state-outside/fixture"):
            assert False
    except PermissionError:
        scope_denials += 1
    try:
        with file = open(directory + "-outside/fixture", "w"):
            assert False
    except PermissionError:
        scope_denials += 1
    assert scope_denials == 2
    print("native port grants ok")
