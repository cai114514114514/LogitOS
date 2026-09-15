#!/usr/bin/env python3
"""Require normal controls and the exact geometry failures, not any crash/nonzero.
The expected failure labels were captured from source-v6 before the candidate.
"""
import json,re,subprocess,sys
from pathlib import Path
binary,log,mode=sys.argv[1:]
assert mode in ('current','legacy','old')
expected=json.loads(Path(__file__).with_name('physical_spacing_calc_expected.json').read_text())
p=subprocess.run([binary,'literal'],capture_output=True,text=True)
literal=p.stdout+p.stderr
assert p.returncode==0 and re.search(r'physical-spacing-calc: %d checks, 0 failures'%expected['literal_checks'],literal),literal
p=subprocess.run([binary],capture_output=True,text=True)
out=p.stdout+p.stderr
Path(log).parent.mkdir(parents=True,exist_ok=True)
Path(log).write_text(literal+'\n'+out)
match=re.search(r'physical-spacing-calc: (\d+) checks, (\d+) failures',out)
assert match and int(match[1])==expected['checks'],out
failed=[s.strip()[6:].split(' -- got ')[0] for s in out.splitlines() if s.strip().startswith('FAIL: ')]
if mode=='current':
    assert p.returncode==0 and int(match[2])==0 and not failed,out
else:
    assert p.returncode==1 and int(match[2])==len(expected['legacy_failures']),out
    assert sorted(failed)==sorted(expected['legacy_failures']), 'unexpected failure matrix: '+out
print('%s: literal %d/%d; %s'%(mode,expected['literal_checks'],expected['literal_checks'],match[0]))
