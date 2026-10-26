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
    # SDK tools are not catalogue apps. lslcc.c and olscc.c have a main() under
    # c/apps/coreutils, but tests/openlogit_sdk.mk builds them as native ELF files
    # that reach the disk through the sysroot ($(BUILD)/sysroot:/ puts them at
    # /bin/lslcc), and nothing builds a $(BUILD)/<name>.aex for them. Registering
    # them would make test-agent-catalog-build and run-agent.py's catalogue boot
    # demand an .aex that does not exist, so they stay absent. The set is read
    # from the SDK's own rules rather than listed here: a new SDK tool is exempt
    # because of the rule that builds it, and only that.
    sdk=set(re.findall(r'^\$\(BUILD\)/sdk/[\w-]+\.o:\s*(c/apps/\S+\.c)',(ROOT/'tests/openlogit_sdk.mk').read_text(),re.M))
    # Every unregistered entry point is reported, not just the first. On
    # 2026-10-26 seventeen had accumulated (the coreutils/text tools, spotlight
    # and the two SDK compilers) while this assertion named only lslcc.c.
    missing=[]
    for p in sorted((ROOT/'c/apps').rglob('*.c')):
        rel=str(p.relative_to(ROOT))
        if 'browser' in p.parts or 'libc' in p.parts or rel in sdk:continue
        if re.search(r'\b(?:int|void)\s+(?:main|app_main)\s*\(',p.read_text(errors='replace')) and rel not in registered:missing.append(rel)
    assert not missing, 'missing owned apps: '+', '.join(missing)
    print(f'AGENT_CATALOG_OK owned_entrypoints={len(catalog)}')
if __name__=='__main__':main()
