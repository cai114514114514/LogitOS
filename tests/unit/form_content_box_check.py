import collections
import pathlib
import re
import subprocess
import sys

run = subprocess.run([sys.argv[1]], capture_output=True, text=True)
pathlib.Path(sys.argv[2]).write_text(run.stdout + run.stderr)
print(run.stdout, end='')
want = []
if sys.argv[3] == 'old':
    for case in ['reset16', 'focused-reset16', 'reset20', 'small-padding']:
        want += [case + ': ' + message for message in [
            'paint uses declared content edges',
            'placeholder em fits vertical clip',
            'placeholder starts at content origin']]
    for case in ['asymmetric', 'percent240', 'percent320']:
        want += [case + ': ' + message for message in [
            'paint uses declared content edges',
            'placeholder starts at content origin']]
    want += ['authored-natural: natural width includes content and edges once'] * 3
    want += ['authored-natural: natural height includes content and edges once'] * 3
    want += ['short-selection: selection uses computed content origin',
             'short-selection: caret shares the selection content origin',
             'short-selection: click uses the same computed origin',
             'short-inner-scroll: End caret reserves space at computed right edge',
             'short-inner-scroll: End caret stays inside declared content clip',
             'padding-repaint: right padding change participates in paint signature']
actual = re.findall(r'^FAIL: (.+)$', run.stdout, re.M)
assert collections.Counter(actual) == collections.Counter(want), (actual, want)
assert re.search(rf'^form-content-box: 68 checks, {len(want)} failures$', run.stdout, re.M)
assert run.returncode == (1 if want else 0)
