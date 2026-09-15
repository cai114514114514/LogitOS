#!/usr/bin/env python3
"""Require decoding/oracle controls and exactly the old stylesheet pixel defects."""
import json,re,subprocess,sys
from pathlib import Path
binary,log,mode=sys.argv[1:]
assert mode in ('current','legacy','old')
e=json.loads(Path(__file__).with_name('svg_stylesheet_expected.json').read_text())
p=subprocess.run([binary,'literal'],capture_output=True,text=True)
control=p.stdout+p.stderr
assert p.returncode==0 and re.search(r'svg-stylesheet: %d checks, 0 failures'%e['literal_checks'],control),control
p=subprocess.run([binary],capture_output=True,text=True)
out=p.stdout+p.stderr
Path(log).parent.mkdir(parents=True,exist_ok=True)
Path(log).write_text(control+'\n'+out)
m=re.search(r'svg-stylesheet: (\d+) checks, (\d+) failures',out)
assert m and int(m[1])==e['checks'],out
failed=[s[6:] for s in out.splitlines() if s.startswith('FAIL: ')]
if mode=='current':
    assert p.returncode==0 and int(m[2])==0 and not failed,out
else:
    assert p.returncode==1 and int(m[2])==len(e['legacy_failures']),out
    assert sorted(failed)==sorted(e['legacy_failures']),out
print('%s: literal %d/%d; %s'%(mode,e['literal_checks'],e['literal_checks'],m[0]))
