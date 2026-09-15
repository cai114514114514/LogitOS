#!/usr/bin/env python3
"""Parse the one deterministic ring-3 HTTP result, allowing kernel log inserts.

The current CLI emits length/FNV only after HTTP framing and a 2xx status have
succeeded, followed by the independently computed SHA-256.  The former kernel
``[http] get`` diagnostic belonged to the retired ring-0 client and must not be
required as evidence for this ring-3 path.  Only complete bracketed diagnostics
may occur BETWEEN writes; never join arbitrary lines or decimal fragments.
"""
import argparse
from pathlib import Path
import re

EXPECTED_BYTES = 32768
EXPECTED_FNV = 4213874117
EXPECTED_SHA256 = '611253a4531dea3d840789b4f11a1ad9c4329fbbf85ee1634f2ae601e6da6db0'
# These are actual kernel diagnostic producers observed during guest CLI writes.
# Keep the whitelist explicit; unknown output causes a visible failure.
DIAG = r'(?:\[(?:mm|netlock|wm|time|sched|pcache|reclaim|rmap|swap|ip6|tcp)\] [^\r\n]*\r?\n)*'
RESULT = re.compile(r'^http bytes '+DIAG+r'(?P<bytes>[0-9]+)'+DIAG+
                    r' fnv1a '+DIAG+r'(?P<fnv>[0-9]+)'+DIAG+r'\r?\n', re.MULTILINE)
SHA256 = re.compile(r'^sha256 '+DIAG+r'(?P<sha>[0-9a-f]{64})'+DIAG+r'\r?\n', re.MULTILINE)

def parse_result(text):
    records=list(RESULT.finditer(text))
    if len(records)!=1:
        raise ValueError('missing or ambiguous complete guest HTTP result')
    result={'bytes':int(records[0]['bytes']), 'fnv1a':int(records[0]['fnv'])}
    if result['bytes']!=EXPECTED_BYTES or result['fnv1a']!=EXPECTED_FNV:
        raise ValueError('guest HTTP payload length/checksum mismatch: '+str(result))
    hashes=list(SHA256.finditer(text,records[0].end()))
    if len(hashes)!=1 or hashes[0]['sha']!=EXPECTED_SHA256:
        raise ValueError('missing, ambiguous, or mismatched guest HTTP SHA-256')
    result['sha256']=hashes[0]['sha']
    return result

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('log',type=Path);a=p.parse_args()
    try:r=parse_result(a.log.read_text(errors='replace'))
    except ValueError as e:raise SystemExit('FAIL: '+str(e))
    print(f"PASS HTTP bytes={r['bytes']} fnv1a={r['fnv1a']} sha256={r['sha256']}")
