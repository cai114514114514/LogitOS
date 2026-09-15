#!/usr/bin/env python3
"""Install the native graphics SDK after the libc sysroot is complete."""
import argparse
import hashlib
import json
import pathlib
import re
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

headers = [root / 'c/lib/gfx/include/gfx.h',
           root / 'c/apps/gui/openlogit_window.h', root / 'c/apps/logit.h',
           root / 'c/apps/text_metrics_wiring.inc']
headers += sorted((root / 'c/lib/gfx3d/include').glob('openlogit*.h'))
headers += [h for h in (root / 'c/lib/gfx/include').glob('openlogit*.h') if h.name != 'openlogit_sw.h']
headers += list((root / 'include/abi').glob('*.h'))
# A public header's quoted includes are part of the installed ABI too, even
# when named .inc. The host's broad -I set hid a missing text-metrics include;
# only guest compilation exposed it. Fail packaging before producing that SDK.
header_names = {h.name for h in headers}
if len(header_names) != len(headers):
    raise SystemExit('OpenLogit SDK header basename collision')
for src in headers:
    for name in re.findall(r'^\s*#\s*include\s*"([^"]+)"', src.read_text(), re.M):
        if name not in header_names:
            raise SystemExit(f'OpenLogit SDK missing public dependency: {src.name} -> {name}')
for src in headers:
    install(src, pathlib.Path('usr/include/openlogit') / src.name)
install(a.build / 'sdk/libopenlogit.a', pathlib.Path('usr/lib/libopenlogit.a'))
install(a.build / 'sdk/island.elf', pathlib.Path('bin/island'))
install(a.build / 'sdk/olscc.elf', pathlib.Path('bin/olscc'))
install(a.build / 'sdk/lslcc.elf', pathlib.Path('bin/lslcc'))
install(a.build / 'sdk/materials.elf', pathlib.Path('bin/materials'))
install(a.build / 'sdk/scene-studio.elf', pathlib.Path('bin/scene-studio'))
install(a.build / 'sdk/vector-studio.elf', pathlib.Path('bin/vector-studio'))
example_root = root / 'examples/openlogit'
for src in sorted(example_root.rglob('*')):
    if src.is_file():
        install(src, pathlib.Path('usr/share/openlogit') / src.relative_to(example_root))
manifest = a.sysroot / 'usr/share/openlogit/manifest.json'
manifest.write_text(json.dumps({'api': '1.1', 'backend': 'software', 'gpu': False, 'files': installed}, indent=2)+'\n')
print(f'OpenLogit SDK 1.1: installed {len(installed)} files, software 2D/3D, GPU unavailable')
