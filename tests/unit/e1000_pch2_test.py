#!/usr/bin/env python3
"""Production 82579 PCH2 probe/MMIO/DMA tests; no emulated or physical NIC claim."""
import argparse,os,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--build',required=True,type=Path)
p.add_argument('--controls-only',action='store_true')
p.add_argument('--positive-only',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
controls=[('IGNORE_FW','managed firmware is explicitly refused without PHY reset'),
          ('STEAL_SWFLAG','busy SWFLAG is neither stolen nor cleared'),
          ('WRONG_PHY_ADDR','complete non-managed 82579 probe succeeds'),
          ('NO_MASTER_DRAIN','unacknowledged master drain retains every DMA buffer'),
          ('ACCEPT_RX_ERROR','RX hardware errors are never delivered'),
          ('OFFLINE_NO_ACK','offline retained IRQ acknowledges pending without scheduling')]
variants=[] if a.controls_only else [('positive',None)]
if not a.positive_only:variants+=controls
for name,expected in variants:
    exe=b/name
    cmd=[os.environ.get('CC','cc'),'-std=c11','-D_POSIX_C_SOURCE=200112L','-O1','-g','-Wall','-Wextra','-Werror',
         '-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all']
    for d in ['c/drivers/net','c/drivers/core','c/kernel/pci','c/kernel/core','c/net/core']:cmd+=['-I'+str(root/d)]
    if expected:cmd+=['-DPCH2_NEGCTL_'+name]
    cmd+=[str(root/'tests/unit/e1000_pch2_test.c'),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    res=subprocess.run([str(exe)],capture_output=True,text=True,timeout=30)
    (b/(name+'.log')).write_text(res.stdout+res.stderr)
    if expected:
        if res.returncode!=1 or 'FAIL: '+expected not in res.stdout or res.stderr:
            raise RuntimeError(name+' failed for wrong reason\n'+res.stdout+res.stderr)
        print('EXPECTED-FAIL PCH2 '+name+': '+expected)
    elif res.returncode:raise RuntimeError(res.stdout+res.stderr)
    else:print(res.stdout.strip())
