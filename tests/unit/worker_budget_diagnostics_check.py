"""Check numeric-only Worker time-rail evidence from the local apparatus."""
import re
import sys
from pathlib import Path
lines = Path(sys.argv[1]).read_text().splitlines()
raw = [s for s in lines if s.startswith('[runtime-diag] worker-budget')]
pattern = re.compile(r'\[runtime-diag\] worker-budget id=\d+ phase=eval elapsed=(\d+) polls=(\d+) max-gap=(\d+) rail=time')
rows = [pattern.fullmatch(s) for s in raw]
ok = len(rows) == 1 and all(rows) and int(rows[0][1]) > 8000 and int(rows[0][2]) > 0 and int(rows[0][3]) >= 1000
jobs = [s for s in lines if s.startswith('[runtime-diag] worker-job')]
job_pattern = re.compile(r'\[runtime-diag\] worker-job id=\d+ elapsed=(\d+) polls=(\d+) outcome=complete begin=\d+')
job_rows = [job_pattern.fullmatch(s) for s in jobs]
ok = ok and len(job_rows) == 1 and all(job_rows) and int(job_rows[0][1]) >= 100 and int(job_rows[0][2]) > 0
page_rows = [s for s in lines if s.startswith('[runtime-diag] page-job')]
ok = ok and any(re.fullmatch(r'\[runtime-diag\] page-job begin=\d+ elapsed=250 outcome=complete', s) for s in page_rows)
ok = ok and all(re.fullmatch(r'\[runtime-diag\] page-job begin=\d+ elapsed=\d+ outcome=(complete|error)', s) for s in page_rows)
ok = ok and 'worker-budget-diagnostics: 4 checks, 0 failures' in lines
print(('ok: ' if ok else 'FAIL: ') + 'Worker budget rail has bounded numeric execution metadata')
sys.exit(0 if ok else 1)
