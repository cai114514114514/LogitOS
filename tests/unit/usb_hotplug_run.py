#!/usr/bin/env python3
"""Build the real USB core with a watched no-CSC negative control."""
import argparse,os,subprocess
from pathlib import Path

p=argparse.ArgumentParser();p.add_argument('--build',type=Path,required=True)
p.add_argument('--negative-only',action='store_true');p.add_argument('--positive-only',action='store_true')
a=p.parse_args();root=Path(__file__).resolve().parents[2];a.build.mkdir(parents=True,exist_ok=True)
variants=[('positive',[])]
negative=[('ignore-csc',['-DUSB_HOTPLUG_NEGCTL_IGNORE_CSC']),
          ('unlocked-detach',['-DUSB_HOTPLUG_NEGCTL_UNLOCKED_DETACH'])]
if a.negative_only:variants=negative
elif not a.positive_only:variants.extend(negative)
for name,flags in variants:
    out=(a.build/name).resolve();out.mkdir(parents=True,exist_ok=True)
    core=(root/'c/drivers/usb/usb_core.c').read_text().split('/* xHCI adapter.')[0]
    core=core.replace('#include "../core/io_lock.h"','#include "io_lock.h"')
    core=core.replace('#include "../core/io_domain.h"','#include "io_domain.h"')
    core=core.replace('#include "sched.h"','void sched_poll_wait(void);')
    (out/'core.c').write_text(core)
    cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-pthread',
         '-fsanitize=address,undefined',*flags,
         '-Ic/drivers/usb','-Ic/drivers/core','-Ic/kernel/pci','-Ic/kernel/core -Ic/kernel/init -Ic/kernel/diag -Ic/kernel/sync','-Ic/drivers/timer',
         'tests/unit/usb_hotplug_test.c',str(out/'core.c'),'c/drivers/usb/usb_bind.c',
         'c/drivers/usb/usb_desc.c','-o',str(out/'test')]
    subprocess.run(cmd,cwd=root,check=True)
    run=subprocess.run([str(out/'test')],cwd=root,capture_output=True,text=True,timeout=30,
                       env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
    (out/'result.log').write_text(run.stdout+run.stderr)
    if name=='positive':
        assert run.returncode==0,(run.stdout,run.stderr)
        assert 'usb-hotplug: 12 checks, 0 failures' in run.stdout,run.stdout
        print(run.stdout.strip(),flush=True)
    else:
        marker={'ignore-csc':'FAIL: change interrupt without CSC leaves existing device untouched',
                'unlocked-detach':'FAIL: disconnect waits for in-flight class callback before remove'}[name]
        assert run.returncode==1 and marker in run.stdout,(run.returncode,run.stdout,run.stderr)
        assert 'AddressSanitizer' not in run.stderr and 'runtime error:' not in run.stderr
        print(f'EXPECTED-FAIL usb-hotplug {name}: {marker[6:]}',flush=True)
