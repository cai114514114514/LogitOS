#!/usr/bin/env python3
"""Install the native graphics SDK after the libc sysroot is complete."""
import argparse
import hashlib
import json
import pathlib
import shutil

p = argparse.ArgumentParser()
p.add_argument('--sysroot', required=True, type=pathlib.Path)
p.add_argument('--build', required=True, type=pathlib.Path)
a = p.parse_args()
root = pathlib.Path(__file__).resolve().parent.parent
installed = []

def install(src, dst):
    out = a.sysroot / dst
    out.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(src, out)
    installed.append({'path': str(dst), 'sha256': hashlib.sha256(out.read_bytes()).hexdigest()})

headers = [root / 'c/lib/gfx/gfx.h', root / 'c/lib/gfx3d/openlogit_3d.h',
           root / 'c/apps/gui/openlogit_window.h', root / 'c/apps/logit.h']
headers += [h for h in (root / 'c/lib/gfx').glob('openlogit*.h') if h.name != 'openlogit_sw.h']
headers += list((root / 'include/abi').glob('*.h'))
for src in headers:
    install(src, pathlib.Path('usr/include/openlogit') / src.name)
install(a.build / 'sdk/libopenlogit.a', pathlib.Path('usr/lib/libopenlogit.a'))
install(a.build / 'sdk/island.elf', pathlib.Path('bin/island'))
install(a.build / 'sdk/olscc.elf', pathlib.Path('bin/olscc'))
for src in sorted((root / 'examples/openlogit').iterdir()):
    if src.is_file():
        install(src, pathlib.Path('usr/share/openlogit') / src.name)
manifest = a.sysroot / 'usr/share/openlogit/manifest.json'
manifest.write_text(json.dumps({'api': '1.1', 'backend': 'software', 'gpu': False, 'files': installed}, indent=2)+'\n')
print(f'OpenLogit SDK 1.1: installed {len(installed)} files, software 2D/3D, GPU unavailable')
