"""An old decode-per-pass policy must lose efficiency, never pixel correctness."""
import sys
from collections import Counter
from pathlib import Path
log=Path(sys.argv[1]).read_text()
expected=[
    'unchanged reflow avoids another decoder call',
    'outer viewport reflow reuses identical raster input',
    'unrelated DOM change reuses unchanged SVG',
    'outer SVG opacity does not need another raster',
    'unchanged CSS-sized raster reuses exact pixels',
    'passive document also reuses unchanged input',
    'restoring parent retains its own raster cache',
    'identical separate SVG nodes share exact serialized pixels',
]
actual=Counter(s.removeprefix('FAIL: ') for s in log.splitlines() if s.startswith('FAIL: '))
assert actual==Counter(expected),(actual,expected)
assert 'svg-reflow-cache: 34 checks, 8 failures;' in log
print('svg-reflow negative: exact 8 cache-hit failures; all pixel/invalidation controls preserved')
