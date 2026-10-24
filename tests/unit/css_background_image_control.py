#!/usr/bin/env python3
"""Controls must execute and name actual pixel/cache failures."""
import subprocess
import sys
from pathlib import Path

for binary in sys.argv[1:]:
    result = subprocess.run([binary], capture_output=True, text=True)
    Path(binary + '.log').write_text(result.stdout + result.stderr)
    required = 'cover center crop samples the intrinsic picture'
    assert result.returncode == 1, (binary, result.returncode, result.stderr)
    assert 'FAIL: ' + required in result.stdout, (binary, result.stdout)
    if binary.endswith('no-load'):
        assert 'FAIL: completed background uses the actual shared decoded cache' in result.stdout
    print(Path(binary).name + ': observed real cache/pixel failures')
