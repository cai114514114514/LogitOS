#!/usr/bin/env python3
"""The fixed normal-input oracle was verified against real source-v6."""
import json,re,subprocess,sys
from pathlib import Path
from collections import Counter
binary,log,mode=sys.argv[1:]
expected=json.loads(Path(__file__).with_name('physical_spacing_boundary_expected.json').read_text())
p=subprocess.run([binary],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT)
Path(log).write_text(p.stdout)
summary=re.findall(r'physical spacing boundaries: (\d+) checks, (\d+) failures',p.stdout)
failures=Counter(line.split('FAIL: ',1)[1].split(' -- got ',1)[0] for line in p.stdout.splitlines() if 'FAIL: ' in line)
want=Counter(expected['legacy_failures']) if mode=='legacy' else Counter()
assert p.returncode==(1 if want else 0),(p.returncode,p.stdout)
assert summary==[(str(expected['checks']),str(sum(want.values())))],summary
assert failures==want,{'extra':dict(failures-want),'missing':dict(want-failures)}
print(f'physical-spacing-boundary {mode}: {expected["checks"]} checks, exact {sum(want.values())} failures')
