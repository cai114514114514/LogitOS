#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Generate one deliberate guest control under BUILD; never mutate shared sources."""
import argparse,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--control',choices=['identity','low-pages','module-high'],required=True);a=p.parse_args()
r=Path(__file__).resolve().parents[2];out=a.out.resolve();out.mkdir(parents=True,exist_ok=True)
for rel in ['c/kernel/mm','c/kernel/module','include/weaksym.h']:
    dst=out/rel;dst.parent.mkdir(parents=True,exist_ok=True)
    if (r/rel).is_dir():shutil.copytree(r/rel,dst,dirs_exist_ok=True)
    else:shutil.copy2(r/rel,dst)
file='c/kernel/module/modload.c' if a.control=='module-high' else 'c/kernel/mm/phys/kheap.c'
old,new={'identity':('low ? mm_p2v(phys) : mm_physmap_ptr(phys)','mm_p2v(phys)'),
         'low-pages':('pmm_alloc_contig_masked(frames, UINT64_MAX, FRAME_SIZE, 0)','pmm_alloc_contig(frames)'),
         'module-high':('uint8_t *blk = kmalloc_low((size_t)need)','uint8_t *blk = kmalloc((size_t)need)')}[a.control]
source=(r/file).read_text()
if source.count(old)!=1:raise SystemExit('Control expression drifted')
(out/file).write_text(source.replace(old,new))
