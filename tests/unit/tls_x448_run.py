#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""X448's real TLS consumers: 12 new cells plus 12 unchanged X25519 controls."""
import argparse
import os
from pathlib import Path
import re
import socket
import subprocess

ROOT = Path(__file__).resolve().parents[2]
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--build', type=Path, required=True)
a = p.parse_args()
a.build.mkdir(parents=True, exist_ok=True)
for negative in (False, True):
    name = 'negative' if negative else 'positive'
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]
    env = {**os.environ, 'BUILD': str((a.build / name).resolve()),
           'TLS_MATRIX_GROUPS': 'X25519 X448', 'TLS_MATRIX_PORT': str(port),
           'TLS_MATRIX_CFLAGS': '-DLOGIT_TLS_NO_X448' if negative else ''}
    result = subprocess.run(['bash', 'tests/unit/run-tls-matrix.sh'], cwd=ROOT,
        env=env, capture_output=True, text=True, timeout=180)
    (a.build / (name + '.log')).write_text(result.stdout + result.stderr)
    rows = re.findall(r'^([AB])\s+(12|13)\s+\S+\s+(X25519|X448)\s+(ok|FAIL)\s',
                      result.stdout, re.M)
    assert len(rows) == 24, f'HARNESS: expected 24 actual cells, got {len(rows)}; see {a.build}/{name}.log'
    assert result.returncode == int(negative), f'incorrect matrix status: {name}'
    for direction, version, group, verdict in rows:
        expected = 'FAIL' if negative and group == 'X448' else 'ok'
        assert verdict == expected, (name, direction, version, group, verdict)
    print(f'TLS X448 {name}: ' + ('12 X448 cells fail; 12 X25519 cells still pass' if negative else
          '24 version/suite/direction cells pass, including verified application data'), flush=True)
print('CONTROL FIRED: removing the X448 offer removes exactly its 12 consumers')
