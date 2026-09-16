# aether: 3.0
# Expected authority comes from the guest harness's kernel grant, not from
# asking the runtime what result the test ought to expect.
def main() -> None:
    expected = parse_int(args()[1])
    assert caps().bits() == expected
    if (expected & CAP_RAW) != 0:
        unsafe:
            memory = alloc(8)
            try:
                memory[0] = 253
                assert memory[0] == 253 and memory[7] == 0
            except Error as error:
                dealloc(memory)
                raise error
            dealloc(memory)
    else:
        denied = False
        unsafe:
            try:
                alloc(8)
            except PermissionError:
                denied = True
        assert denied
    print("native allocation grants ok")
