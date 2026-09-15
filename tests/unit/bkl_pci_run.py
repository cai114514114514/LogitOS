#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real PCI/IOAPIC code; only register and page-mapping leaves are emulated."""
from pathlib import Path
import argparse,os,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');ap.add_argument('--x2apic-only',action='store_true');ap.add_argument('--source-root',type=Path);a=ap.parse_args()
r=(a.source_root or Path(__file__).resolve().parents[2]).resolve();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
test=r/'tests/unit/bkl_pci_test.c'
if a.x2apic_only:
 variants=[('initmask-positive',5,''),('badver-positive',6,''),
           ('initmask',5,'IOAPIC init rejects an unconfirmed initial mask before clearing destination'),
           ('badver',6,'malformed IOAPIC VER is rejected before any RTE write')]
else:
 variants=[('port',0,'CF8/CFC address-data pairs'),('rmw',1,'8-bit writes preserve'),('rmw16',2,'16-bit writes preserve'),('map',3,'one mapper publishes'),('publish',3,'ECAM pointers are published'),('ioapic',4,'IOAPIC selector-window')] if a.negative_only else [('positive',i,'') for i in range(5)]
for name,mode,marker in variants:
 d=b/(name+str(mode));d.mkdir(exist_ok=True)
 pci=(r/'c/kernel/pci/pci.c').read_text();io=(r/'c/kernel/cpu/irq/ioapic.c').read_text()
 for old,new in [('"../../drivers/core/io_lock.h"',r/'c/drivers/core/io_lock.h'),('"../../drivers/core/io_domain.h"',r/'c/drivers/core/io_domain.h')]:
  pci=pci.replace(old,'"'+str(new)+'"');io=io.replace(old,'"'+str(new)+'"')
 if name=='port':
  for fn in ['uint32_t pci_cfg_read(','void pci_cfg_write(']:
   pos=pci.index(fn);j=pci.index('    IO_GUARD(&cfg_gate);',pos);pci=pci[:j]+pci[j:].replace('    IO_GUARD(&cfg_gate);','/* control: pair unprotected */',1)
 if name in ['rmw','rmw16']:
  # Restore split dword read/modify/write, rather than remove native-byte
  # locking: two actual byte writes to one dword do not lose each other.
  bits=8 if name=='rmw' else 16
  start=pci.index('void pci_cfg_write'+str(bits)+'(')
  end=pci.index('\n}',start)+2
  replacement=('void pci_cfg_write%d(uint8_t bus,uint8_t slot,uint8_t func,uint16_t off,uint%d_t val)\n'
   '{ uint16_t d=off&~3u; uint32_t v=pci_cfg_read(bus,slot,func,d);\n'
   '  unsigned sh=(off&%d)*8,mask=%su<<sh;\n'
   '  pci_cfg_write(bus,slot,func,d,(v&~mask)|((uint32_t)val<<sh)); }') % (bits,bits,3 if bits==8 else 2,'255' if bits==8 else '65535')
  pci=pci[:start]+replacement+pci[end:]
 if name=='map':
  start=pci.index('static volatile uint8_t *ecam_ptr(');j=pci.index('        IO_DOMAIN_GUARD(&ecam_owner);',start);pci=pci[:j]+pci[j:].replace('        IO_DOMAIN_GUARD(&ecam_owner);','/* control: concurrent mappers */',1)
 if name=='publish':
  old='            vmm_map_range(p, p, 1u << 20, VMM_WRITABLE | VMM_NOCACHE);\n            __atomic_store_n(&g_ecam_mapped[bus], 1, __ATOMIC_RELEASE);'
  assert old in pci;pci=pci.replace(old,'            __atomic_store_n(&g_ecam_mapped[bus], 1, __ATOMIC_RELEASE);\n            vmm_map_range(p, p, 1u << 20, VMM_WRITABLE | VMM_NOCACHE);')
 if name=='ioapic':io=io.replace('    IO_GUARD(&ioapic_gate);','/* control: no controller gate */')
 if name=='initmask':
  io='#define LOGIT_X2APIC_NEGCTL_SKIP_INIT_MASK_READBACK\n'+io
 if name=='badver':
  io='#define LOGIT_X2APIC_NEGCTL_ALLOW_INVALID_IOAPIC_VER\n'+io
 # Preserve production transaction bodies and substitute only MMIO leaf access.
 io=io.replace('*(volatile uint32_t *)(io + 0) = reg;', 'test_ioapic_select(reg);').replace('return *(volatile uint32_t *)(io + 0x10);','return test_ioapic_load();').replace('*(volatile uint32_t *)(io + 0x10) = v;', 'test_ioapic_store(v);')
 io='void test_ioapic_select(unsigned); void test_ioapic_store(unsigned); unsigned test_ioapic_load(void);\n'+io
 # Read-only observer of production readiness; transaction bodies are intact.
 pci+='\nint test_ecam_ready(unsigned bus) { return __atomic_load_n(&g_ecam_mapped[bus], __ATOMIC_ACQUIRE); }\n'
 (d/'pci.c').write_text(pci);(d/'ioapic.c').write_text(io)
 (d/'vmm.h').write_text('#include <stdint.h>\n#define VMM_WRITABLE 2\n#define VMM_NOCACHE 24\nvoid vmm_map_page(uint64_t,uint64_t,uint64_t);\nvoid vmm_map_range(uint64_t,uint64_t,uint64_t,uint64_t);\n')
 exe=d/'test';cmd=[os.environ.get('CC','clang'),'-std=gnu11','-O1','-g','-pthread','-fsanitize=address,undefined','-DLOGIT_HOST_TEST','-I'+str(d)]+['-I'+str(r/p) for p in ['tests/unit/pcistub','c/drivers/core','c/kernel/pci','c/kernel/cpu','c/kernel/cpu/acpi','c/kernel/cpu/irq','c/kernel/cpu/smp']]
 subprocess.run(cmd+[str(test),str(d/'pci.c'),str(d/'ioapic.c'),
                     str(r/'c/kernel/cpu/irq/apic_model.c'),'-o',str(exe)],check=True)
 p=subprocess.run([str(exe),str(mode)],capture_output=True,text=True,timeout=25);(d/'result.log').write_text(p.stdout+p.stderr)
 if marker:
  assert p.returncode==1 and 'FAIL: '+marker in p.stdout,(name,p.stdout,p.stderr)
  if a.x2apic_only:
   failed=[line for line in p.stdout.splitlines() if line.startswith('FAIL: ')]
   assert len(failed)==1 and marker in failed[0],(name,failed,p.stderr)
 else:assert p.returncode==0,(name,p.stdout,p.stderr)
 print(name+': '+p.stdout.strip().splitlines()[-1])
