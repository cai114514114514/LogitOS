import re,subprocess,sys
from pathlib import Path
binary,log,mode=sys.argv[1:];r=subprocess.run([binary],stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True);Path(log).write_text(r.stdout)
want=0 if mode=='current' else 23
m=re.search(r'form-native-button-edges: (\d+) checks, (\d+) failures',r.stdout)
assert m and tuple(map(int,m.groups()))==(80,want),(r.returncode,r.stdout)
assert r.returncode==(0 if want==0 else 1),r.returncode
print(f'form-native-button-edges {mode}: 80 checks / {want} expected failures')
