"""Require the native enqueue, dispatch, and completion clocks independently."""
from pathlib import Path
import re
import sys

lines = Path(sys.argv[1]).read_text().splitlines()
rows = [s for s in lines if s.startswith('[runtime-diag] port-timing')]
pattern = re.compile(r'\[runtime-diag\] port-timing id=\d+ seq=\d+ queued-at=100 begin=350 elapsed=23')
ok = len(rows) == 1 and pattern.fullmatch(rows[0]) and 'port-timing: PASS' in lines
print(('ok: ' if ok else 'FAIL: ') + 'port queue residence differs from callback duration')
sys.exit(0 if ok else 1)
