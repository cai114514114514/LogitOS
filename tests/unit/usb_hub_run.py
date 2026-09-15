#!/usr/bin/env python3
"""Actual core/hub/binder; only controller/clock/PCI leaves are substituted."""
import argparse,os,subprocess
from pathlib import Path
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');a=ap.parse_args()
r=Path(__file__).resolve().parents[2]
variants=[('',None)] if not a.negative_only else [('USB_HUB_NEGCTL_NO_CHILDREN','nested hub boot enumerates both leaf devices'),('USB_HUB_NEGCTL_NO_TT','nested leaf inherits nearest HS hub translator port'),('USB_HUB_NEGCTL_CONTROL_ONE_DIRECTION','control error clears both TT directions')]
for macro,marker in variants:
    b=a.build.resolve()/(macro.lower() if macro else 'positive');b.mkdir(parents=True,exist_ok=True)
    core=(r/'c/drivers/usb/usb_core.c').read_text().split('/* xHCI adapter.')[0]
    core=core.replace('#include "../core/io_lock.h"','#include "io_lock.h"').replace('#include "../core/io_domain.h"','#include "io_domain.h"')
    core=core.replace('#include "sched.h"','void sched_poll_wait(void);').replace('#include "xhci.h"','')
    (b/'core.c').write_text(core)
    cmd=[os.environ.get('CC','clang'),'-O1','-g','-Wall','-Wextra','-pthread','-fsanitize=address,undefined',
        '-Ic/drivers/usb','-Ic/drivers/core','-Ic/kernel/pci','-Ic/kernel/core -Ic/kernel/init -Ic/kernel/diag -Ic/kernel/sync','-Ic/drivers/timer',
        'tests/unit/usb_hub_test.c',str(b/'core.c'),'c/drivers/usb/usb_hub.c','c/drivers/usb/usb_bind.c','c/drivers/usb/usb_desc.c','-o',str(b/'test')]
    if macro:cmd.insert(1,'-D'+macro)
    subprocess.run(cmd,cwd=r,check=True)
    p=subprocess.run([str(b/'test')],capture_output=True,text=True,timeout=20);(b/'result.log').write_text(p.stdout+p.stderr)
    if marker:assert p.returncode==1 and 'FAIL: '+marker in p.stdout,(p.stdout,p.stderr)
    else:assert p.returncode==0,(p.stdout,p.stderr)
    print((macro or 'positive')+': '+p.stdout.strip().splitlines()[-1])
