import re
import sys
from pathlib import Path

xhr, worker = [Path(p).read_text() for p in sys.argv[1:3]]
xl = [s for s in xhr.splitlines() if s.startswith('[runtime-diag]')]
wl = [s for s in worker.splitlines() if s.startswith('[runtime-diag]')]
checks = [
    ('xhr diagnostic metadata observable', bool(xl)),
    ('worker startup failure observable', any('phase=fetch kind=failed' in s for s in wl)),
    ('worker module constructor rejection observable', any('phase=constructor kind=module-unsupported' in s for s in wl)),
    ('worker origin refusal observable', any('phase=start kind=origin-rejected' in s for s in wl)),
    ('worker evaluated and reached running', any('phase=eval kind=complete' in s for s in wl) and any('phase=start kind=running' in s for s in wl)),
    ('integer JSON codes observable', any('phase=json parse=ok code=int:0 biz_code=int:104' in s for s in xl)),
    ('noninteger JSON codes categorized', any('phase=json parse=ok code=non_integer biz_code=non_integer' in s for s in xl)),
    ('missing fields remain missing', any('phase=json parse=ok code=missing biz_code=missing' in s for s in xl)),
    ('invalid JSON categorized without parser message', any('phase=json parse=invalid' in s for s in xl)),
    ('native TypeError classification observable', any('phase=callback-error event=progress type=TypeError' in s for s in xl)),
    ('Proxy classified without properties', any('phase=callback-error event=progress type=object' in s for s in xl)),
    ('diagnostic payloads are bounded fixed fields', not any('PRIVATE_' in s or 'http' in s or '/' in s for s in xl + wl)),
    ('non-JSON and oversized bodies unparsed', not any('int:7351' in s or 'int:8351' in s or 'int:9351' in s for s in xl)),
    ('terminal event counters observable', any(re.search(r'phase=terminal progress=\d+ load=1 error=0 abort=0 received=\d+ chars=\d+$', s) for s in xl)),
    ('XHR and native fetch ids correlate', any(re.search(r'xhr id=[1-9]\d* fetch=\d+ phase=headers status=200 mime=json$', s) for s in xl)),
]
for name, ok in checks:
    print(('ok  : ' if ok else 'FAIL: ') + name)
failures = sum(not ok for _, ok in checks)
print(f'runtime-diagnostics: {len(checks)} checks, {failures} failures')
sys.exit(bool(failures))
