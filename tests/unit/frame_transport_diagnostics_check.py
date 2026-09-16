"""Check native transport traces, without accepting author-controlled text."""
import re
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text(errors='replace').splitlines()
traces = [line for line in lines if line.startswith('[runtime-diag]')]
frame = re.compile(r'\[runtime-diag\] frame slot=\d+ up=[01] phase=(attempt|clone-refused|no-recipient|queued|drop-stale|drop-origin|owner-busy|dispatch) bytes=\d+')
port = re.compile(r'\[runtime-diag\] port id=\d+ phase=(start|clone-refused|peer-closed|queued|dispatch) bytes=\d+ started=[01] queued=\d+')
port_error = re.compile(r'\[runtime-diag\] port-error kind=(transfer-not-array|transfer-capacity|transfer-not-owned-port|data-serialization|byte-budget)')
port_budget = re.compile(r'\[runtime-diag\] port-budget bytes=\d+ retained=\d+ limit=\d+ total=\d+')
frame_input = re.compile(r'\[runtime-diag\] frame-input slot=\d+ event=(down|up) x=-?\d+ y=-?\d+ hit=\d+ checkbox=\d+ allowed=[01] click=[01] at=\d+')
port_timing = re.compile(r'\[runtime-diag\] port-timing id=\d+ seq=\d+ queued-at=\d+ begin=\d+ elapsed=\d+')
page_job = re.compile(r'\[runtime-diag\] page-job begin=\d+ elapsed=\d+ outcome=(complete|error)')
checks = {
    'native frame transport observable': any('frame slot=' in x and 'phase=queued' in x for x in traces),
    'native window message dispatch observable': any('frame slot=' in x and 'phase=dispatch' in x for x in traces),
    'native port transport observable': any('port id=' in x and 'phase=queued' in x for x in traces),
    'native port dispatch observable': any('port id=' in x and 'phase=dispatch' in x for x in traces),
    # This fixture actually routes native down/up through passive_frame_pointer.
    # Merely accepting the new trace syntax would pass if input logging vanished.
    'native embedded click observable': any(frame_input.fullmatch(x) and 'event=up ' in x and 'click=1' in x for x in traces),
    'trace contains only bounded metadata': bool(traces) and all(frame.fullmatch(x) or port.fullmatch(x) or port_error.fullmatch(x) or port_budget.fullmatch(x) or frame_input.fullmatch(x) or port_timing.fullmatch(x) or page_job.fullmatch(x) for x in traces),
    'instrumentation keeps actual consumer working': 'passive-frame active-ports: PASS' in lines,
}
for name, passed in checks.items():
    print(('ok: ' if passed else 'FAIL: ') + name)
sys.exit(not all(checks.values()))
