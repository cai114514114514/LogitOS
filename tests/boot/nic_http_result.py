#!/usr/bin/env python3
"""Parse the one deterministic guest HTTP result, allowing kernel log inserts.

The CLI emits five writes: prefix, decimal length, label, decimal hash, newline.
Only complete bracketed kernel diagnostics may occur BETWEEN those writes.
Never join arbitrary lines or split decimal fragments into a successful result.
"""
import argparse
from pathlib import Path
import re

EXPECTED_BYTES = 32768
EXPECTED_FNV = 4213874117
# These are actual kernel diagnostic producers observed during guest CLI writes.
# Keep the whitelist explicit; unknown output causes a visible failure.
DIAG = r'(?:\[(?:mm|netlock|wm|time|sched|pcache|reclaim|rmap|swap|ip6|tcp)\] [^\r\n]*\r?\n)*'
RESULT = re.compile(r'^http bytes '+DIAG+r'(?P<bytes>[0-9]+)'+DIAG+
                    r' fnv1a '+DIAG+r'(?P<fnv>[0-9]+)'+DIAG+r'\r?\n', re.MULTILINE)

def parse_result(text):
    status = re.search(r'\[http\] get rc=0 status=2(?:\s|$)', text)
    if status is None:
        raise ValueError('missing actual guest HTTP rc=0 status=2')
    records=list(RESULT.finditer(text,status.end()))
    if len(records)!=1:
        raise ValueError('missing or ambiguous complete guest HTTP result')
    result={'bytes':int(records[0]['bytes']), 'fnv1a':int(records[0]['fnv'])}
    if result['bytes']!=EXPECTED_BYTES or result['fnv1a']!=EXPECTED_FNV:
        raise ValueError('guest HTTP payload length/checksum mismatch: '+str(result))
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);a=p.parse_args()
    try:r=parse_result(a.log.read_text(errors='replace'))
    except ValueError as e:raise SystemExit('FAIL: '+str(e))
    print(f"PASS HTTP bytes={r['bytes']} fnv1a={r['fnv1a']}")
