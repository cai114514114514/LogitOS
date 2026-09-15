#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Recover immutable application inputs from an accepted disk into a private BUILD.

Kernel work must not rebuild protected browser/third-party sources just because
another task removed shared build artifacts. Reuse the repository's filesystem
reader, match the current make manifest, and record every recovered byte hash.
"""
import argparse,hashlib,importlib.util,json,re,subprocess
from pathlib import Path
from make_disk_recipe import disk_recipe
p=argparse.ArgumentParser();p.add_argument('--disk',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
if out==r or out==r/'build':raise SystemExit('Use a private application BUILD')
previous=out/'apps-snapshot.json'
if previous.exists():
    for rel in json.loads(previous.read_text())['recovered_sha256']:
        dest=(out/rel).resolve()
        if not dest.is_relative_to(out):raise SystemExit('Invalid previous staging path')
        if dest.is_file():dest.unlink()
run=subprocess.run(['make','-n','-W','tools/mkfs.py','BUILD='+str(out),str(out/'disk.img')],cwd=r,capture_output=True,text=True)
(out/'apps.make').write_text(run.stdout)
try: _,specs=disk_recipe(run.stdout)
except ValueError as error:raise SystemExit('Invalid disk recipe: '+str(error)+'; '+run.stderr[-2000:])
spec=importlib.util.spec_from_file_location('license_reader',r/'tools/license_audit.py');module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module)
fs=module._LogitFS(a.disk);copied={}
def restore(ino,dest):
    if fs._inode(ino)['type']==2:
        dest.mkdir(parents=True,exist_ok=True)
        for name,kid in fs._readdir(ino).items():
            if name in ('.','..') or '/' in name:raise SystemExit('Invalid snapshot directory entry')
            restore(kid,dest/name)
    else:
        data=fs.read_file(ino);dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data)
        copied[str(dest.relative_to(out))]=hashlib.sha256(data).hexdigest()
try:
    # Preservation arguments describe the old disk, not files to recover.
    for item in specs:
        host,guest=item.split(':',1) if ':' in item else (item,'/'+Path(item).name)
        path=Path(host)
        if not path.is_absolute():path=r/path
        path=path.resolve()
        if not path.is_relative_to(out):
            if not path.exists():raise SystemExit('Missing external manifest input: '+str(path))
            continue
        if guest=='/':
            # mksysroot.py writes exactly usr/include and usr/lib. Extracting
            # the entire guest root here duplicates all separately packed apps
            # and would import old test fixtures into new acceptance disks.
            if path.name!='sysroot':raise SystemExit('Unknown root-directory spec '+item)
            for sub in ['usr/include','usr/lib']:
                ino=fs.lookup('/'+sub)
                if ino is None:raise SystemExit('Snapshot lacks sysroot '+sub)
                restore(ino,path/sub)
            continue
        ino=fs.lookup(guest)
        if ino is None:raise SystemExit('Snapshot lacks '+guest+'; no substitute fabricated')
        restore(ino,path)
finally:fs.close()
(out/'apps-snapshot.json').write_text(json.dumps(dict(source_disk=str(a.disk.resolve()),disk_sha256=hashlib.sha256(a.disk.read_bytes()).hexdigest(),recovered_sha256=copied),indent=2)+'\n')
print('Restored',len(copied),'application inputs from accepted disk to',out)
