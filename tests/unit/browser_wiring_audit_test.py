#!/usr/bin/env python3
"""Delete a required call in a temporary source overlay; never touch product.

The linked binary is intentionally unchanged: if an audit equates 'symbol
exists' with 'wired', it will pass this control and the gate will reject it.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--elf', default='build/browser.elf')
p.add_argument('--out', default='build/wiring')
a = p.parse_args()
root = Path(__file__).resolve().parents[2]
relative = 'c/apps/browser/browser.c'
source = (root/relative).read_text()
broken, count = re.subn(r'js_cssom_set_reflow\s*\(browser_cssom_reflow\)\s*;',
                        '(void)0;', source)
if count != 1:
    sys.exit('wiring control: expected exactly one real reflow registration, found '+str(count))
with tempfile.TemporaryDirectory(prefix='logitos-wiring-control-') as tmp:
    overlay = Path(tmp)/'browser.c'
    overlay.write_text(broken)
    result = subprocess.run([sys.executable, str(root/'tools/browser_wiring_audit.py'),
        '--elf', a.elf, '--json', str(Path(a.out)/'negative-control.json'),
        '--source-override', relative+'='+str(overlay)], cwd=root, text=True, capture_output=True)
    print(result.stdout, end='')
    print(result.stderr, end='')
    failures = [line for line in result.stdout.splitlines() if line.startswith(('MISSING_', 'EMPTY_', 'ELF_UNAVAILABLE', 'NM_FAILED'))]
    if result.returncode != 1 or len(failures) != 1 or not failures[0].startswith('MISSING_CALL cssom_reflow '):
        sys.exit('wiring control: FAIL -- required call removal did not produce the exact missing-call failure')
print('wiring control: PASS -- unchanged linked symbol did not hide the removed consumer call')
