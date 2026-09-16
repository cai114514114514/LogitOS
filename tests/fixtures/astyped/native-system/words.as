# aether: 3.0
# A private host runtime captures words instead of contacting a host kernel.
sequence = 0

def word(value: i64) -> i64:
    global sequence
    sequence = sequence * 10 + value
    return value

def main() -> None:
    unsafe:
        assert syscall(9) == 73
        assert syscall(11, -7) == -123
        high: u64 = 18446744073709551615
        assert syscall(12, 0, high) == 17
        assert syscall(word(1), word(2), word(3), word(4)) == 99
        assert sequence == 1234
    print("native syscall words ok")
