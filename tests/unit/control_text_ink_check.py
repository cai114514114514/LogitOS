import re,subprocess,sys
from pathlib import Path
binary,log,mode=sys.argv[1:];r=subprocess.run([binary],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);Path(log).write_text(r.stdout)
want=0 if mode=='current' else 92
m=re.search(r'control-text-ink: (\d+) checks, (\d+) failures',r.stdout)
assert m and tuple(map(int,m.groups()))==(995,want),(r.returncode,r.stdout)
assert r.returncode==(0 if want==0 else 1),r.returncode
print(f'control-text-ink {mode}: 995 checks / {want} expected failures')
