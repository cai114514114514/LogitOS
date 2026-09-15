#!/usr/bin/env python3
"""Exact finite normal-behavior controls; no crash or timeout is an accepted red."""
import collections, pathlib, re, subprocess, sys
exe, log, mode = sys.argv[1:]
run = subprocess.run([exe], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=45)
pathlib.Path(log).write_text(run.stdout)
print(run.stdout, end='')
want = []
if mode == 'old':
    for scene in ('explicit-lines', 'fractional-lines', 'normal-lines'):
        want += [f'{scene}: {s}' for s in ('text x uses containing element line context', 'text y uses document root line context', 'repaint does not accumulate resolved lengths')]
    want += ['inherited-line: text x uses containing element line context', 'inherited-line: repaint does not accumulate resolved lengths']
    for scene in ('lh-back', 'lh-quarter', 'rlh-back'):
        want += [f'{scene}: {s}' for s in ('face ink matches facing', 'ordinary descendant shares face visibility', 'face background follows same visibility', 'native hit agrees with visible plane', 'covered link only returns when face is absent', 'full repaint repeats current facing')]
    want += ['line-transform-syntax: supports accepts element-line units', 'line-transform-syntax: supports accepts root-line units', 'line-transform-stacking: line-relative transform establishes stacking context', 'root-line-change: initial rlh resolves root line not root font', 'root-line-change: root line style update reaches transform', 'root-line-change: resize keeps line units independent of viewport']
elif mode == 'cssom-old':
    want = ['computed line units match absolute reference', 'same-turn element line mutation updates matrix', 'same-turn root line mutation updates matrix', 'display-none serializes line lengths but keeps percent', 'normal lines retain existing used strut metrics', 'CSS supports validates line unit grammar']
elif mode not in ('current', 'cssom'):
    raise SystemExit('unknown mode')
actual = re.findall(r'^FAIL: (.+)$', run.stdout, re.M)
count = 9 if mode.startswith('cssom') else 89
prefix = 'transform-lineheight-cssom' if mode.startswith('cssom') else 'transform-lineheight'
assert collections.Counter(actual) == collections.Counter(want), (actual, want)
assert run.returncode == (1 if want else 0), run.returncode
assert re.search(rf'^{prefix}: {count} checks, {len(want)} failures$', run.stdout, re.M), 'summary missing'
assert 'Sanitizer' not in run.stdout and 'runtime error:' not in run.stdout
print(f'Accepted exact {mode} matrix: {count} assertions, {len(want)} expected failures')
