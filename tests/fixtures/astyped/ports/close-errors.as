# aether: 3.0
# The host test makes every physical close consume its fd and return IOError.
# Print the required close order before unwinding; the C hook records actual
# descriptor closure independently, so wrong order cannot satisfy this oracle.

def fail() -> None:
    raise ValueError("original")

def leave(path: str) -> i64:
    with file = open(path):
        print("close", file.fd())
        try:
            return 7
        except IOError:
            # This try is already exited when the containing owner is closed.
            return 99

def main() -> None:
    path = args()[1]
    caught = False
    try:
        answer = leave(path)
        assert False
    except IOError:
        caught = True
    assert caught

    # The writer's close failure must still release the reader. An original
    # body exception keeps precedence over errors from either endpoint.
    for unwind in [False, True]:
        caught = False
        try:
            with reader, writer = pipe():
                print("close", writer.fd())
                print("close", reader.fd())
                if unwind:
                    fail()
        except ValueError as error:
            assert unwind and error.message == "original"
            caught = True
        except IOError:
            assert not unwind
            caught = True
        assert caught

    caught = False
    try:
        with outer = open(path):
            with inner = open(path):
                print("close", inner.fd())
                print("close", outer.fd())
                fail()
    except ValueError as error:
        assert error.message == "original"
        caught = True
    assert caught

    caught = False
    try:
        with outer = open(path):
            with inner = open(path):
                print("close", inner.fd())
                print("close", outer.fd())
    except IOError:
        caught = True
    assert caught

    with file = open(path):
        print("close", file.fd())
        caught = False
        try:
            file.close()
        except IOError:
            assert file.closed()
            caught = True
        assert caught
        # This close and the generated release must both skip the consumed fd.
        file.close()
    print("close errors ok")
