#!/usr/bin/env python3
"""Actual driver host adapters: only leaf MMIO/PIO access is modelled.

No emulated packet pipeline, codec graph, PCI interrupt controller, or USB device
is claimed. Hardware delivery is checked separately by the existing QEMU gates.
Every allocation point is failed before enable; stopped and stuck devices exercise
ownership lifetimes. Controls mutate driver code, never the check predicates.
"""
from pathlib import Path
import argparse, os, platform, re, subprocess
ap=argparse.ArgumentParser()
ap.add_argument('--repo',type=Path,default=Path(__file__).resolve().parents[2])
ap.add_argument('--build',type=Path,required=True)
ap.add_argument('--negative-only',action='store_true')
ap.add_argument('--asan',action='store_true')
a=ap.parse_args();r=a.repo.resolve();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
tests=Path(__file__).resolve().parent
incs=[p for base in ['c','include'] for p in (r/base).rglob('*') if p.is_dir() and '/apps/' not in str(p) and '/third_party/' not in str(p)]
names=['net/e1000','net/rtl8139','net/rtl8169','usb/xhci','audio/hda']
mode_counts=[76,7,56,6,5]
summary=[]
for kind,name in enumerate(names,1):
 variants=(['address','lifetime']+(['pci-command'] if kind<=3 else [])+(['capability'] if kind==3 else [])+(['rx-overflow'] if kind==2 else [])) if a.negative_only else ['positive']
 for variant in variants:
    d=b/f'{kind}-{variant}';d.mkdir(exist_ok=True)
    txt=(r/f'c/drivers/{name}.c').read_text()
    # Leaf MMIO reads/writes only; the complete production logic is unchanged.
    txt,nread=re.subn(r'return \*\(volatile uint(8|16|32)_t\s*\*\)\(([^;]+)\);',lambda m:f'return test_read({m[2]}, {int(m[1])//8});',txt)
    txt,nwrite=re.subn(r'\*\(volatile uint(8|16|32)_t\s*\*\)\(([^;]+)\) = v;',lambda m:f'test_write({m[2]}, {int(m[1])//8}, v);',txt)
    assert (nread,nwrite)==[(1,1),(0,0),(3,3),(1,1),(3,3)][kind-1],(name,nread,nwrite)
    if kind==4:
        assert 'x->db[slot] = target;' in txt
        txt=txt.replace('x->db[slot] = target;', 'test_write(&x->db[slot], 4, target);')
    if variant=='address':
        if kind<=3:old,new='dma_addr_value(b->dma)','(uint64_t)(uintptr_t)b->cpu'
        if kind==2:old,new='dma_addr_value(tx_dma[i]->dma)','(uint64_t)(uintptr_t)txbuf[i]'
        if kind==4:old,new='dma_addr_value(dma_addr_add(b->dma, p - base))','p'
        if kind==5:old,new='dma_addr_value(h->ring_dma->dma)','(uint64_t)(uintptr_t)h->ring'
        assert old in txt;txt=txt.replace(old,new)
    if variant=='lifetime':
        dev='&nic_dma' if kind<=3 else ('&x->dma' if kind==4 else '&h->dma')
        old=f'dma_device_quarantine({dev});'
        assert old in txt
        new=f'dma_device_quiesced({dev}); while (({dev})->buffers) dma_free_coherent(({dev})->buffers);'
        txt=txt.replace(old,new)
    if variant=='capability':
        old='rtl_dma_mask(dev, r32(R_TCR))';assert old in txt
        txt=txt.replace(old,'DMA_MASK_64')
    if variant=='rx-overflow':
        old='if (isr & (ISR_RXOVW | ISR_FOVW)) g_rx_overflow++;'
        assert old in txt
        txt=txt.replace(old,'if (isr & (ISR_RXOVW | ISR_FOVW)) rx_reset();')
    if variant=='pci-command':
        old='if (dev_enable_checked(dev, 0) != 0) {'
        assert txt.count(old)==1
        txt=txt.replace(old,'if (0 && dev_enable_checked(dev, 0) != 0) {')
    (d/'dma_driver.inc').write_text(txt)
    cmd=[os.environ.get('CC','clang'),'-O1','-g','-ffunction-sections','-fdata-sections','-DLOGIT_HOST_TEST',f'-DDRIVER_KIND={kind}','-Wall','-Wextra','-o',str(d/'test'),str(tests/'dma_driver_test.c'),'-I'+str(d),'-I'+str(tests/'dma_driver_stub')]+['-I'+str(p) for p in incs]
    if platform.system()=='Darwin':cmd+=['-arch','x86_64','-Wl,-dead_strip']
    else:cmd+=['-Wl,--gc-sections']
    if a.asan:cmd+=['-fsanitize=address,undefined']
    if kind==4:cmd+=[str(r/'c/drivers/usb/xhci_ring.c')]
    subprocess.run(cmd,check=True)
    modes=range(mode_counts[kind-1]) if variant=='positive' else ([0] if variant=='address' else ([52] if variant=='capability' else [5 if kind==4 else (2 if kind==5 else 1)]))
    for mode in modes:
        argv=[str(d/'test'),str(mode)]
        if variant=='pci-command':argv.append('command-reject')
        p=subprocess.run(argv,capture_output=True,text=True,timeout=20)
        (d/f'mode-{mode}.log').write_text(p.stdout+p.stderr)
        if variant=='positive':assert p.returncode==0,(name,mode,p.stdout,p.stderr)
        else:assert p.returncode==1 and 'FAIL ' in p.stdout,(name,variant,'control not detected',p.stdout,p.stderr)
    if variant=='positive' and kind<=3:
        p=subprocess.run([str(d/'test'),'0','command-reject'],capture_output=True,text=True,timeout=20)
        (d/'command-reject.log').write_text(p.stdout+p.stderr)
        assert p.returncode==0,(name,'command rejection path',p.stdout,p.stderr)
    summary.append(f'{name}: {variant}, {len(modes)} cases PASS')
print('\n'.join(summary))
