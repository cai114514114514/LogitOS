#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Keep the product file list, replacing only the rebuilt smptest fixture."""
from pathlib import Path
import re
import subprocess
import sys

root=Path(__file__).resolve().parents[2]
build=Path(sys.argv[1]).resolve()
source=(build/'bkl-disk.make').read_text()
# mkfs deliberately rejects duplicate destinations. Replace this existing
# file specification before adding the two new fixtures; never copy an old
# smptest from the shared application build after changing its timing clock.
text,count=re.subn(r'(?<!\S)\S+:/bin/smptest(?=\s|$)',
                  lambda _:str(build/'smptest.aex')+':/bin/smptest',source)
if count!=1:raise SystemExit('expected one /bin/smptest specification, got '+str(count))
manifest=build/'bkl-disk-fixtures.make';manifest.write_text(text)
subprocess.run([sys.executable,'tests/boot/mk-tcc-disk.py','.',str(manifest),
                str(build/'bkl-disk.img'),str(build/'bkl.aex')+':/bin/bkl',
                str(build/'sigtest.aex')+':/bin/sigtest'],cwd=root,check=True)
