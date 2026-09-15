"""Observe only the existing seven normal arithmetic checks, with the flag off/on."""
import pathlib
import re
import sys

off, on = (pathlib.Path(p).read_text().splitlines() for p in sys.argv[1:])
for lines in (off, on):
    assert "worker-wasm-normal: 7 checks, 0 failures" in lines
    assert not any(line.startswith("FAIL:") or "[js exception]" in line for line in lines)
prefix = "[runtime-diag] wasm-call "
assert not any(line.startswith(prefix) for line in off), "default build emitted Wasm diagnostics"
records = [line for line in on if line.startswith(prefix)]
assert len(records) == 12, "six ordinary calls must produce six begin/end pairs"
begins = []
ends = set()
for line in records:
    begin = re.fullmatch(r"\[runtime-diag\] wasm-call id=(\d+) phase=begin", line)
    if begin:
        begins.append(int(begin[1]))
        continue
    end = re.fullmatch(r"\[runtime-diag\] wasm-call id=(\d+) phase=end elapsed_ms=(\d+) ok=1 failed=0", line)
    assert end, f"unexpected fields or result in diagnostic record: {line}"
    call = int(end[1])
    assert call in begins and call not in ends
    ends.add(call)
assert begins == list(range(1, 7)) and ends == set(begins)
print("worker-wasm-diagnostics: ordinary 7/7 with flag off and on; six matching metadata-only call pairs")
