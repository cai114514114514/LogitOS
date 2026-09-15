#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
import argparse,pathlib,subprocess,tempfile,sys,json,re
ROOT=pathlib.Path(__file__).resolve().parents[2]
def run(define,build):
    name=define or 'positive';exe=build/name
    sources=['tests/unit/agent_core_test.c','c/lib/agent/task.c','c/lib/agent/store.c','c/lib/agent/json.c','c/lib/agent/model_json.c','c/drivers/block/crc32.c']
    command=['clang','-std=c11','-O1','-g','-fsanitize=address,undefined','-fno-omit-frame-pointer','-o',str(exe),*sources]
    if define:command+=['-D'+define]
    compiled=subprocess.run(command,cwd=ROOT,capture_output=True,text=True)
    if compiled.returncode: raise SystemExit(compiled.stdout+compiled.stderr)
    with tempfile.TemporaryDirectory(prefix='ag-core-') as directory:
        return subprocess.run([str(exe),directory],capture_output=True,text=True)
def main():
    p=argparse.ArgumentParser();p.add_argument('--build',required=True,type=pathlib.Path);p.add_argument('--negative',action='store_true');a=p.parse_args();a.build.mkdir(parents=True,exist_ok=True)
    if a.negative:
        for define,label in [('AG_NEG_PERMISSION','permission:'),('AG_NEG_REVISION','revision:'),('AG_NEG_DEDUP','dedup:'),('AG_NEG_CHECKPOINT','checkpoint:')]:
            r=run(define,a.build.resolve());text=r.stdout+r.stderr
            if r.returncode!=1 or 'FAIL: '+label not in text:raise SystemExit('INCONCLUSIVE '+define+'\n'+text)
            print(define+': '+next(x for x in text.splitlines() if x.startswith('FAIL:')))
        return
    r=run('',a.build.resolve());print(r.stdout+r.stderr,end='');r.check_returncode()
    sys.path.insert(0,str(ROOT/'tools'));from agent_catalog import entries
    catalog=entries();registered={x['source'] for x in catalog}
    for p in (ROOT/'c/apps').rglob('*.c'):
        if 'browser' in p.parts or 'libc' in p.parts:continue
        if re.search(r'\b(?:int|void)\s+(?:main|app_main)\s*\(',p.read_text(errors='replace')):
            assert str(p.relative_to(ROOT)) in registered, f'missing owned app: {p}'
    print(f'AGENT_CATALOG_OK owned_entrypoints={len(catalog)}')
if __name__=='__main__':main()
