# aether: 3.0
# Write directly to the LogitOS descriptor ABI. The message has a known byte
# length, so no implicit C-string conversion or host syscall table is involved.
def main() -> None:
    message = Bytes("hello via syscall\n")
    unsafe:
        assert syscall(SYS_WRITE, 1, addr(message), len(message)) == len(message)
