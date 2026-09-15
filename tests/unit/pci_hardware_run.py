#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Checks real PCI accessors; controls restore the two former hardware defects."""
import argparse, os, subprocess
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--build',required=True,type=Path);p.add_argument('--negative-only',action='store_true');a=p.parse_args()
r=Path(__file__).resolve().parents[2];out=a.build.resolve();out.mkdir(parents=True,exist_ok=True)
variants=[('wide-write','ports','Command write preserves W1C Status'),('bus-relative','ecam','MCFG base remains relative to bus zero'),('all-edge','intx','physical PCI stays level-triggered')] if a.negative_only else [('positive','ports',''),('positive','ecam',''),('positive','intx','')]
for name,mode,marker in variants:
 d=out/(name+'-'+mode);d.mkdir(exist_ok=True)
 s=(r/'c/kernel/pci/pci.c').read_text()
 for h in ['io_lock.h','io_domain.h']:
  s=s.replace('"../../drivers/core/'+h+'"','"'+str(r/'c/drivers/core'/h)+'"')
 if name=='wide-write':
  start=s.index('void pci_cfg_write16(');end=s.index('\n}',start)+2
  s=s[:start]+'''void pci_cfg_write16(uint8_t bus,uint8_t slot,uint8_t func,uint16_t off,uint16_t val)
{ uint16_t d=off&~3u;unsigned sh=(off&2)*8;uint32_t old=pci_cfg_read(bus,slot,func,d);
  pci_cfg_write(bus,slot,func,d,(old&~(65535u<<sh))|((uint32_t)val<<sh)); }'''+s[end:]
 if name=='bus-relative':
  old='((uint64_t)bus << 20)';assert s.count(old)==2
  s=s.replace(old,'((uint64_t)(bus - g_ecam_bus_lo) << 20)')
 (d/'pci.c').write_text(s)
 platform=(r/'c/kernel/pci/pci_platform.h').read_text()
 if name=='all-edge':
  platform=platform.replace('maxleaf < 0x40000000u) return 1;', 'maxleaf < 0x40000000u) return 0;')
 (d/'pci_platform.h').write_text(platform)
 (d/'vmm.h').write_text('#include <stdint.h>\n#define VMM_WRITABLE 2\n#define VMM_NOCACHE 24\nvoid vmm_map_range(uint64_t,uint64_t,uint64_t,uint64_t);\n')
 exe=d/'test'
 cmd=[os.environ.get('CC','clang'),'-std=c11','-O1','-g','-Wall','-Wextra','-fsanitize=address,undefined','-DLOGIT_HOST_TEST','-I'+str(d)]
 cmd+=['-I'+str(r/x) for x in ['tests/unit/pcistub','c/drivers/core','c/kernel/pci']]
 subprocess.run(cmd+[str(r/'tests/unit/pci_hardware_test.c'),str(d/'pci.c'),'-o',str(exe)],check=True)
 run=subprocess.run([str(exe),mode],capture_output=True,text=True,timeout=15)
 text=run.stdout+run.stderr;(d/'result.log').write_text(text);print(text,end='')
 assert 'runtime error:' not in text and 'AddressSanitizer' not in text,text
 if a.negative_only:
  assert run.returncode==1 and 'FAIL: '+marker in text,(name,run.returncode,text)
  print('negative control OK:',name)
 else:assert run.returncode==0,(name,run.returncode,text)
