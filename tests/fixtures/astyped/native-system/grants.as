# aether: 3.0
def main() -> None:
    expected = parse_int(args()[1])
    assert caps().bits() == expected
    allowed = (expected & CAP_RAW) != 0
    denied = 0
    unsafe:
        try:
            assert mem2str(Bytes("ok"), 2) == "ok"
            assert allowed
        except PermissionError:
            assert not allowed
            denied += 1
        try:
            # This query has no resource-category requirement in the kernel.
            assert syscall(SYS_MONOTONIC_MS) >= 0
            assert allowed
        except PermissionError:
            assert not allowed
            denied += 1
    assert denied == (0 if allowed else 2)
    print("native system grants ok")
