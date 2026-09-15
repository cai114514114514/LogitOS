#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real VFS pthread race tests. Faulty source variants must fail named checks."""
from pathlib import Path
import argparse, os, subprocess
ap=argparse.ArgumentParser()
ap.add_argument('--build',type=Path,required=True)
ap.add_argument('--negative-only',action='store_true')
a=ap.parse_args();r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
s=(r/'c/fs/vfs.c').read_text()
variants=['no-owner','no-refs'] if a.negative_only else ['positive']
for name in variants:
    text=s
    if name=='no-owner':
        old='if (ref.m && owned) { io_domain_enter(&ref.m->op); ref.owned = 1; }'
        assert text.count(old)==1
        text=text.replace(old,'(void)owned; /* injected missing transaction owner */')
    if name=='no-refs':
        old='__atomic_fetch_add(&ref.m->refs, 1, __ATOMIC_RELAXED)'
        assert text.count(old)==2
        text=text.replace(old,'(void)0').replace('__atomic_fetch_sub(&ref->m->refs, 1, __ATOMIC_RELEASE)','(void)0')
    src=b/(name+'.c');src.write_text(text)
    binary=b/name
    cmd=[os.environ.get('CC','clang'),'-O1','-g','-Wall','-Wextra','-pthread','-fsanitize=address,undefined',
         '-I'+str(r/'tests/unit'),'-I'+str(r/'c/fs'),'-I'+str(r/'c/kernel/core'),
         str(r/'tests/unit/bkl_vfs_test.c'),str(src),str(r/'c/fs/vfs_meta.c'),
         str(r/'c/fs/vfs_path.c'),str(r/'c/fs/ramfs.c'),'-o',str(binary)]
    subprocess.run(cmd,check=True)
    p=subprocess.run([str(binary)],capture_output=True,text=True,timeout=30)
    (b/(name+'.log')).write_text(p.stdout+p.stderr)
    if name=='positive':
        assert p.returncode==0,(p.stdout,p.stderr)
    else:
        expected={'no-owner':'8000 names survive backend','no-refs':'unmount must wait for active mount borrower'}[name]
        assert p.returncode==1 and 'FAIL: '+expected in p.stdout,(name,p.stdout,p.stderr)
    print(name+': '+p.stdout.strip().splitlines()[-1])
