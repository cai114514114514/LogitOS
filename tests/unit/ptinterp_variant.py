#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate deliberate controls in a private build tree only."""
import argparse, hashlib, json
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--out',type=Path,required=True)
p.add_argument('--control',choices=['areas','metadata','fork-fp','stack'],required=True)
a=p.parse_args();root=Path(__file__).resolve().parents[2]
file,old,new={
 'areas':('c/kernel/exec/load/elf.c','if (deferred && LOGIT_HAVE(vma_reserve_fixed)) {','if (0) {'),
 'metadata':('c/kernel/exec/load/elf.c','if (deferred && LOGIT_HAVE(vma_reserve_fixed) &&','if (0 &&'),
 'fork-fp':('c/boot/enter_user.asm','    fxrstor [r13]','    fninit ; control: omit inherited user state'),
 'stack':('c/kernel/exec/load/exec.c','    if (need > stack_pages) need = stack_pages;','    /* control: minimum ignores one-page GUI hint */'),
}[a.control]
source=(root/file).read_text()
if source.count(old)!=1:raise SystemExit('Control expression drifted: '+a.control)
# Absolute quoted includes preserve the real source's header resolution.
if file.endswith('.c'):
 import re
 source=re.sub(r'^#include "([^"]+)"',lambda m:'#include "'+str(((root/file).parent/m[1]).resolve())+'"' if ((root/file).parent/m[1]).exists() else m[0],source,flags=re.M)
target=a.out/file;target.parent.mkdir(parents=True,exist_ok=True)
target.write_text(source.replace(old,new))
(a.out/'control.json').write_text(json.dumps(dict(control=a.control,file=file,original_sha256=hashlib.sha256((root/file).read_bytes()).hexdigest(),variant_sha256=hashlib.sha256(target.read_bytes()).hexdigest()),indent=2)+'\n')
