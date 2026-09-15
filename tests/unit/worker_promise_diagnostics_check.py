#!/usr/bin/env python3
"""Strictly fixed diagnostic fields; scripts and fake URLs are local fixtures."""
import re
import sys
from pathlib import Path

lines = Path(sys.argv[1]).read_text().splitlines()
diag = [line for line in lines if line.startswith('[runtime-diag]')]
checks = []
def ck(ok, name):
    checks.append(bool(ok))
    print(('ok  : ' if ok else 'FAIL: ') + name)

pattern = re.compile(r'^\[runtime-diag\] worker id=(\d+) phase=(promise-rejected-provisional|promise-handled) kind=(Error|EvalError|RangeError|ReferenceError|SyntaxError|TypeError|URIError|InternalError|AggregateError|object|string|number|boolean|null|undefined|other) missing-pattern=(none|fetch|WebAssembly|URL|URLSearchParams|TextEncoder|TextDecoder|crypto|atob|btoa|Response|Request|Headers)$')
events = [pattern.fullmatch(line) for line in diag if 'phase=promise-' in line]
ck(bool(events), 'worker promise rejection metadata observable')
ck(all(events), 'promise diagnostic fields contain only fixed enumerations')
rows = [m.groups() for m in events if m]
ck({row[0] for row in rows} == {'1', '2'}, 'tracker ownership isolates both workers and resolved control')
ck(all('LOCAL_ONLY_SECRET' not in line and 'fixture.test' not in line for line in diag), 'diagnostics omit message text and URLs')
for wid in ('1', '2'):
    own = [r for r in rows if r[0] == wid]
    rejected = [r for r in own if r[1] == 'promise-rejected-provisional']
    handled = [r for r in own if r[1] == 'promise-handled']
    ck(len(rejected) == 30 and len(handled) == 29, f'worker {wid} provisional and handled counts preserve async failure')
    ck({r[3] for r in own if r[3] != 'none'} == {'fetch', 'WebAssembly', 'URL', 'URLSearchParams', 'TextEncoder', 'TextDecoder', 'crypto', 'atob', 'btoa', 'Response', 'Request', 'Headers'}, f'worker {wid} exact missing-global allowlist observable')
    ck({'Error', 'EvalError', 'RangeError', 'ReferenceError', 'SyntaxError', 'TypeError', 'URIError', 'InternalError', 'AggregateError', 'object', 'string'} <= {r[2] for r in own}, f'worker {wid} native types and opaque values classified')
    ck(sum(f'worker id={wid} phase=inbound-message-dispatch ' in line for line in diag) == 2 and sum(f'worker id={wid} phase=outbound-message-enqueue ' in line for line in diag) == 3, f'worker {wid} inbound and outbound message stages observable')
print(f'worker-promise-metadata: {len(checks)} checks, {checks.count(False)} failures')
sys.exit(0 if all(checks) else 1)
