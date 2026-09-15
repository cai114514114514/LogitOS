#!/usr/bin/env python3
"""Sanitized production EHCI backend tests and watched fault controls."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--controls-only',action='store_true')
p.add_argument('--positive-only',action='store_true')
a=p.parse_args();r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
controls=[('NO_TT','FS split records actual TT hub and port'),('FULL_SHORT','short packet reports exact actual bytes'),
          ('IGNORE_BIOS','firmware ownership timeout refuses controller'),
          ('NO_HALT','unacknowledged hardware halt is reported before DMA classification'),
          ('PRE_HANDOFF_BME','pre-ownership PCI gate clears bus master and confirms MEM/INTx state'),
          ('COMMAND_NO_READBACK','PCI quiet gate refuses a dropped MEM-decode readback'),
          ('MASTER_BEFORE_HALT','bus master cannot start before an acknowledged controller halt'),
          ('RESTORE_NO_READBACK','unconfirmed PCI Command restore is retained as an unsafe state'),
          ('CLEAR_MASTER_NO_READBACK','teardown retains DMA when bus-master clear is not confirmed'),
          ('POST_HANDOFF_RESTORE_BME','post-semaphore handoff failure retains quiet ownership and blocks reprobe'),
          ('SKIP_ISOLATE_ON_HALT_FAIL','halt-timeout shutdown still isolates BME and retains submitted DMA')]
exact_controls={'POST_HANDOFF_RESTORE_BME','SKIP_ISOLATE_ON_HALT_FAIL'}
variants=[] if a.controls_only else [('positive',None)]
if not a.positive_only:variants+=controls
for name,expected in variants:
    exe=b/name
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function',
         '-fsanitize=address,undefined','-fno-sanitize-recover=all','-pthread']
    for d in ['c/drivers/usb','c/drivers/core','c/kernel/core','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/pci']:cmd+=['-I'+str(r/d)]
    if expected:cmd+=['-DEHCI_NEGCTL_'+name]
    cmd+=[str(r/'tests/unit/ehci_test.c'),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    res=subprocess.run([str(exe)],capture_output=True,text=True,timeout=20)
    (b/(name+'.log')).write_text(res.stdout+res.stderr)
    if expected:
        fail_lines=[line for line in res.stdout.splitlines() if line.startswith('FAIL: ')]
        exact=name not in exact_controls or fail_lines==['FAIL: '+expected]
        if res.returncode!=1 or 'FAIL: '+expected not in res.stdout or res.stderr or not exact:
            raise RuntimeError(name+' control failed for wrong reason\n'+res.stdout+res.stderr)
        print('EXPECTED-FAIL EHCI '+name+': '+expected+
              (' (exactly one)' if name in exact_controls else ''))
    elif res.returncode:raise RuntimeError(res.stdout+res.stderr)
    else:print(res.stdout.strip())
