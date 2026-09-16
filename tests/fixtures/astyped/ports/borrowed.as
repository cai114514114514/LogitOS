# aether: 3.0
# A borrowed wrapper owns its buffering state, never the descriptor. The
# original owner remains responsible for its lifetime and syscall authority.
def main() -> None:
    path = args()[1] + "/borrowed-port.txt"
    with owner = open(path, "rw"):
        descriptor = owner.fd()
        with view = port(descriptor):
            assert view.kind() == "file"
            assert view.write("prefix") == 6
            view.close()
            view.close()
            assert view.closed() and view.fd() == -1
        assert owner.write(" kept") == 5
        try:
            with view = port(descriptor):
                raise ValueError("release only the wrapper")
        except ValueError:
            pass
        assert owner.write(" alive") == 6
    assert file_read(path) == Bytes("prefix kept alive")
    with owner = open(path):
        with view = port(owner.fd()):
            assert view.read_exact(2) == Bytes("pr")
        assert owner.read_exact(4) == Bytes("efix")
    file_write(path, Bytes("first\nsecond\n"))
    with owner = open(path):
        with view = port(owner.fd()):
            assert view.line() == "first"
        assert owner.line() == "second"

    # Repeated borrowed stdio scopes must leave stdout available to the next
    # scope and to ordinary print after the final wrapper has been released.
    for index in range(64):
        with console = port(1):
            assert console.kind() == "tty"
            assert console.write("") == 0
    with console = port(1):
        assert console.write("borrowed stdout\n") == 16
    errors = 0
    for descriptor in [-1, 65536]:
        try:
            with invalid = port(descriptor):
                pass
        except ValueError:
            errors += 1
    with absent = port(65535):
        try:
            absent.write("x")
        except IOError:
            errors += 1
    assert errors == 3
    print("native borrowed ports ok")
