#!/usr/bin/env python3
"""Actual DMA/PMM with ASAN+UBSAN. Broken controls are required before positive."""
import argparse, os, pathlib, subprocess
p=argparse.ArgumentParser();p.add_argument('--build',type=pathlib.Path,required=True);a=p.parse_args()
r=pathlib.Path(__file__).resolve().parents[2];a.build.mkdir(parents=True,exist_ok=True)
flags=['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DMM_HOSTTEST','-DDMA_HOSTTEST']
flags += ['-I'+str(r/x) for x in ['tests/unit/mmstub','c/drivers/core','c/kernel/mm']]
src=[r/x for x in ['tests/unit/dma_test.c','c/drivers/core/dma.c','c/kernel/mm/pmm.c']]
for tag,assertion in [('DMA_NEG_CPU_ADDRESS','CPU_POINTER_ASSERT'),('DMA_NEG_TRUNCATE','HIGH_ADDRESS_ASSERT'),('DMA_NEG_DIRECTION','DIRECTION_ASSERT'),('DMA_NEG_EARLY_FREE','EARLY_FREE_ASSERT'),('',None)]:
    name=tag or 'dma-core';exe=a.build/name
    subprocess.run([*flags,*(['-D'+tag] if tag else []),*map(str,src),'-o',str(exe)],check=True)
    env=dict(os.environ)
    if tag:env['ASAN_OPTIONS']=env.get('ASAN_OPTIONS','')+':detect_leaks=0'
    result=subprocess.run([str(exe)],text=True,capture_output=True,env=env)
    (a.build/(name+'.log')).write_text(result.stdout+result.stderr)
    if tag:
        if result.returncode!=1 or 'FAIL: '+assertion not in result.stdout:raise RuntimeError(name+' did not fail intended assertion\n'+result.stdout+result.stderr)
        # A deliberate early return leaves fixture allocations alive; disable
        # only leak reporting for this harness, not address/undefined checks.
        if 'ERROR: AddressSanitizer' in result.stderr or 'runtime error:' in result.stderr:raise RuntimeError('apparatus error '+name+result.stderr)
        print('PASS negative control:',name,assertion)
    elif result.returncode:raise RuntimeError(result.stdout+result.stderr)
    else:print(result.stdout.strip().splitlines()[-1])
