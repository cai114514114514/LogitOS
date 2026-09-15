"""Production harness assertions must reject truncated 16G guest evidence."""
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'boot'))
import guest_memory as gm
if '--negative' in sys.argv:
    gm.high_ram = lambda value: value == '8G'  # exact former selection bug
checks = failures = 0

def check(ok, name):
    global checks, failures
    checks += 1
    if not ok:
        failures += 1
        print('FAIL:', name)

def refused(fn, name):
    try:
        fn()
    except ValueError:
        check(True, name)
    else:
        check(False, name)

check(gm.ram_bytes('16384M') == 16 << 30, 'equivalent RAM spelling')
for ram in ['16G', '16384M']:
    refused(lambda: gm.require_high_payload(ram, 0x10000, 1048576, 1048576), '16G must reject low-only payload')
    refused(lambda: gm.require_high_payload(ram, 0x100000000, 0, 1048576), '16G must reject absent high-byte completion')
gm.require_high_payload('16G', 0x100000000, 1048576, 1048576)
check(True, 'completed high payload accepted')
gm.require_high_payload('512M', 0x10000, 0, 1048576)
check(True, 'small machine retains low payload path')
valid = b'[widecheck] capacity usable=17000000000 high_free_pages=3900000\n'
check(gm.require_capacity(valid, '16G')['usable_bytes'] == 17000000000, 'guest usable capacity checked')
refused(lambda: gm.require_capacity(valid + valid, '16G'), 'duplicate capacity is ambiguous')
refused(lambda: gm.require_capacity(b'[widecheck] capacity usable=8000000000 high_free_pages=1500000', '16G'), 'truncated guest capacity rejected')
refused(lambda: gm.ram_bytes('16'), 'ambiguous RAM units rejected')
print(f'GUEST_MEMORY: {checks} checks, {failures} failures')
raise SystemExit(1 if failures else 0)
