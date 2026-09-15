#!/usr/bin/env python3
"""Run the real engine without a window; prove key guards with source mutants."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[2]
p=argparse.ArgumentParser();p.add_argument('--negative',action='store_true');p.add_argument('binary',type=Path);p.add_argument('compiler',type=Path);a=p.parse_args()
binary=a.binary.resolve();compiler=a.compiler.resolve()
def run(executable):
    with tempfile.TemporaryDirectory(prefix='studio-gate-') as tmp:
        return subprocess.run([str(executable),str(compiler),tmp],cwd=ROOT,text=True,capture_output=True,timeout=45)
if not a.negative:
    r=run(binary);print(r.stdout,end='');print(r.stderr,end='');raise SystemExit(r.returncode)
sources=sorted((ROOT/'c/apps/studio').glob('*.c'))+[ROOT/s for s in ['c/apps/as/editor/completion.c','c/lib/agent/json.c','c/lib/agent/task.c','c/drivers/block/crc32.c']]
mutations=[('edit.c','!st_boundary(d->text,d->length,caret)||!st_boundary(d->text,d->length,anchor)','0','utf8-boundary'),
           ('runner.c','r->tab==tab&&r->revision==d->revision&&r->source_length==d->length','r->tab==tab&&r->source_length==d->length','stale-diagnostics'),
           ('pairs.c','unsigned close=closing(cp);','unsigned close=0;','pair-open'),
           ('storage.c','if(!exists){st_dispose(d);return -2;}','if(0){st_dispose(d);return -2;}','deleted-draft-not-restored'),
           ('project.c','if(st_forget_drafts(target)<0)goto failed;','if(0)goto failed;','deleted-draft-not-restored'),
           ('engine.c','if(st_checkpoint(d)<0&&st_dirty(d)){','if(st_checkpoint(d)<0){','clean-missing-parent-close')]
with tempfile.TemporaryDirectory(prefix='studio-mutants-') as tmp:
    for filename,old,new,expected in mutations:
        original=ROOT/'c/apps/studio'/filename;text=original.read_text()
        if text.count(old)!=1:raise RuntimeError('mutation setup drift: '+filename)
        mutant=Path(tmp)/filename;mutant.write_text(text.replace(old,new));exe=Path(tmp)/('test-'+filename)
        command=[os.environ.get('CC','clang'),'-std=c11','-D_DEFAULT_SOURCE','-Wno-misleading-indentation','-g','-O1','-fsanitize=address,undefined','-I'+str(ROOT/'c/apps/studio'),str(ROOT/'tests/unit/studio_core_test.c')]
        command += [str(mutant if s==original else s) for s in sources]+['-o',str(exe)]
        subprocess.run(command,cwd=ROOT,check=True,capture_output=True,text=True)
        result=run(exe)
        if result.returncode==0 or 'FAIL '+expected+' at ' not in result.stderr:
            raise RuntimeError('control failed for wrong reason: '+result.stdout+result.stderr)
        print('PASS negative control:',expected,'observed failing')
