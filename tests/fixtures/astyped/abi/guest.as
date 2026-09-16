# aether: 3.0
import abi

def main() -> None:
    assert abi.proc_pid() > 0
    clock = abi.Time()
    assert abi.get_time(clock) == 0
    assert clock.year >= 2020
    seconds = abi.now_s()
    assert seconds >= 0 and seconds < 86400

    # Use an interior string view: its backing bytes continue with a suffix,
    # so passing addr(path) without C-string marshalling opens the wrong name.
    path = ("prefix/state/native-abi" + "suffix").slice(6, 23)
    payload = Bytes("native ABI file")
    assert abi.file_write(path, payload, len(payload)) == len(payload)
    result = buffer(64)
    count = abi.file_read(path, result, len(result))
    assert count == len(payload)
    unsafe:
        assert mem2str(result, count) == "native ABI file"
    stat = abi.Stat()
    assert abi.stat(path, stat, len(stat)) == 0

    calls = [0]
    def poll() -> i64:
        calls[0] += 1
        return 7 if calls[0] == 2 else 0
    assert abi.await_(poll, false, 0, false, false, 0, 1, "local") == 7
    errors = 0
    try:
        abi.await_(lambda: 0, false, 0, false, false, 0, 0, "local")
    except RuntimeError as error:
        assert "timed out" in error.message
        errors += 1
    try:
        abi.await_(lambda: -7, false, 0, true, true, 0, 1, "local")
    except IOError:
        errors += 1
    assert errors == 2
    assert abi.file_delete(path) == 0
    print("native ABI guest ok")
