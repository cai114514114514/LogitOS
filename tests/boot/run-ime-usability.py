#!/usr/bin/env python3
"""Assert keyboard/click -> WM -> IME -> TextEdit -> on-disk UTF-8 bytes.

Uses a private disk copy for each run. The control sends identical nihao keys
with IME disabled, proving the input/save/extraction chain independently.
The extended case also clicks the menu toggle and the ninth popup candidate.
"""
import shutil, subprocess, sys
from pathlib import Path
if len(sys.argv)!=4:sys.exit(__doc__)
iso,disk,out=map(lambda s:Path(s).resolve(),sys.argv[1:]);out.mkdir(parents=True,exist_ok=True)
r=Path(__file__).resolve().parents[2]
for mode,want in [('ascii','nihao '),('ime','你好'),('usability','你好，我爱中国。你好吗？女孩输入法二维码西安')]:
    copy=out/(mode+'.img');shot=out/(mode+'.ppm');saved=out/(mode+'.txt')
    shutil.copyfile(disk,copy)
    with (out/(mode+'.driver.log')).open('w') as log:
        p=subprocess.run([sys.executable,'tests/boot/ime_type.py',str(iso),str(copy),mode,str(shot)],cwd=r,stdout=log,stderr=subprocess.STDOUT,timeout=360)
    if p.returncode:sys.exit(f'FAIL: {mode} driver; see {out/(mode+".driver.log")}')
    subprocess.run([sys.executable,'tests/boot/lfs_extract.py',str(copy),'/untitled.txt',str(saved)],check=True,cwd=r)
    got=saved.read_bytes()
    if got!=want.encode():sys.exit(f'FAIL: {mode}: wanted {want!r}, got {got!r}; disk kept at {copy}')
    print(f'PASS: {mode}: {got.decode()}',flush=True)
    copy.unlink() # private successful test disk only; screenshots/logs/text remain
