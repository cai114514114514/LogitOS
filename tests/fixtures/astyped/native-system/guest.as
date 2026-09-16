# aether: 3.0
# Exercise real kernel return values and all three argument registers using a
# private file. A Port owns its descriptor while syscall uses its borrowed fd.
def main() -> None:
    path = "/state/native-system"
    file_write(path, Bytes("abcdef"))
    with file = open(path, "rw"):
        unsafe:
            assert syscall(SYS_GETPID) > 0
            assert syscall(SYS_MONOTONIC_MS) >= 0
            data = buffer(8)
            assert syscall(SYS_READ, file.fd(), addr(data), 3) == 3
            assert mem2str(data, 3) == "abc"
            assert syscall(SYS_LSEEK, file.fd(), 1, 0) == 1
            assert syscall(SYS_WRITE, file.fd(), addr(Bytes("XY")), 2) == 2
            # Invalid descriptors return the actual negative kernel result;
            # they are not converted to a fabricated success or an exception.
            assert syscall(SYS_READ, -1, addr(data), 1) < 0
    assert file_read(path).decode() == "aXYdef"
    print("native system calls ok")
