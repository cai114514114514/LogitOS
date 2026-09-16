# aether: 3.0
# storchild.as -- the second half of storprobe's test 8: re-read /sp/a.bin
# from a NEW process. If the parent's close did not flush its descriptor,
# this process cannot read the eight committed bytes from its own descriptor.
#
# The A2 version kept alloc(64) in a global because function assignments were
# local. A3 has an explicit main and a Ptr[u8] local, released on both paths.
# The optional path lets the same shipped program test missing/changed input.
def main() -> None:
    arguments = args()
    path = arguments[1] if len(arguments) > 1 else "/sp/a.bin"
    # The kernel expects a C string; A3 str only promises length-delimited UTF-8.
    terminated_path = Bytes(path + "\0")
    count = -1
    valid = 0
    unsafe:
        memory = alloc(64)
        descriptor = -1
        try:
            descriptor = syscall(SYS_OPEN, addr(terminated_path), 0, 0)
            if descriptor >= 0:
                count = syscall(SYS_READ, descriptor, addr(memory), 64)
                syscall(SYS_CLOSE, descriptor, 0, 0)
                descriptor = -1
            if count == 8:
                if i64(memory[0]) == ord("d") and i64(memory[7]) == ord("d"):
                    valid = 1
        except Error as error:
            if descriptor >= 0:
                syscall(SYS_CLOSE, descriptor, 0, 0)
            dealloc(memory)
            raise error
        dealloc(memory)
    print("STORCHILD readback", count, "bytes-ok", valid)
