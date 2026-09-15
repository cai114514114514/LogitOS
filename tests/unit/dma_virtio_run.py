#!/usr/bin/env python3
"""Run real virtio + DMA core against a sparse >4G host device model.

Controls mutate only temporary source copies. Each must finish normally with a
named FAIL assertion: compiler errors/signals are apparatus failures, not proof.
--negative is exposed so make can wire controls as positive prerequisites.
"""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
CONTROLS = {
    'pci-command-ignore': ('virtio.c',
                           '    if (dev_enable_checked(dev, 0) != 0) {',
                           '    if (0 && dev_enable_checked(dev, 0) != 0) {',
                           'PCI Command failure blocks BAR mapping'),
    'wrong-pci-function': ('virtio.c', '    uint16_t devid = dev->device;',
                           '    dev = dev_find_id(VIRTIO_VENDOR, dev->device, NULL);\n    uint16_t devid = dev->device;',
                           'explicit transport retains selected PCI function'),
    'fb-skip-drain': ('fb.c', '        if (!__atomic_load_n(&fb_front_writers, __ATOMIC_SEQ_CST)) return 0;',
                      '        return 0; /* negative control: release under an AP writer */',
                      'stalled CPU writer prevents normal free'),
    'queue-cpu-address': ('virtio.c', 'w64(vd->common, C_QDESC,   dma_addr_value(vq->desc_mem->dma));',
                          'w64(vd->common, C_QDESC, (uint64_t)(uintptr_t)vq->desc);', 'queue register uses physical'),
    'false-quiescence': ('virtio.c', '        if (r8(vd->common, C_STATUS) == 0) {',
                          '        if (1) { /* negative control: ignore reset ACK */',
                          'failed reset quarantines submitted payload'),
    'balloon-cpu-pfn': ('virtio_balloon.c', 'stage[got] = (uint32_t)(f >> PFN_SHIFT);',
                         'stage[got] = (uint32_t)((uint64_t)(uintptr_t)mm_p2v(f) >> PFN_SHIFT);',
                         'balloon payload equals held guest physical PFN'),
    'gpu-cpu-backing': ('virtio_gpu.c', 'a->addr = dma_addr_value(fb_mem->dma);',
                         'a->addr = (uint64_t)(uintptr_t)fb_mem->cpu;',
                         'GPU nested backing is high guest physical'),
}

def run(negative=None):
    with tempfile.TemporaryDirectory(prefix='logitos-dma-virtio-') as td:
        out = Path(td)
        for p in (ROOT/'c/drivers/virtio').glob('*.[ch]'):
            shutil.copy2(p, out/p.name)
        shutil.copy2(ROOT/'c/kernel/gui/fb.c',out/'fb.c')
        if negative:
            name, old, new, evidence = CONTROLS[negative]
            p=out/name; text=p.read_text()
            if text.count(old)!=1:
                raise RuntimeError(f'control anchor drift: {name}: {old}')
            p.write_text(text.replace(old,new))
        dirs=['c/drivers/core','c/kernel/pci','c/kernel/mm','c/kernel/cpu',
              'c/kernel/core','c/drivers/net','c/net/core','c/drivers/timer','c/kernel/gui','c/lib/gfx','c/lib/text','c/drivers/block','include']
        cmd=[os.environ.get('CC','clang'),'-std=c11','-D_DARWIN_C_SOURCE',
             '-DDMA_HOSTTEST','-DMM_HOSTTEST','-DVIRTIO_HOSTTEST','-DLOGIT_NET_HOST','-DFB_DMA_HOSTTEST',
             '-O1','-g','-Wall','-Wextra','-Wno-unused-parameter','-Wno-unused-variable',
             '-fsanitize=address,undefined','-I'+str(out)]
        cmd += ['-I'+str(ROOT/d) for d in dirs]
        cmd += [str(ROOT/'tests/unit/dma_virtio_test.c'),str(ROOT/'c/drivers/core/dma.c'),str(out/'fb.c'),str(ROOT/'c/kernel/gui/glass.c'),*[str(p) for p in sorted((ROOT/'c/lib/gfx').glob('*.c'))],'-lm','-o',str(out/'test')]
        subprocess.run(cmd,check=True,cwd=ROOT)
        env=dict(os.environ);env['ASAN_OPTIONS']='detect_leaks=0'
        r=subprocess.run([str(out/'test')],text=True,capture_output=True,env=env)
        log=r.stdout+r.stderr
        print(log,end='')
        if negative:
            if r.returncode!=1 or 'FAIL: '+evidence not in log or 'AddressSanitizer' in log or 'runtime error:' in log:
                raise RuntimeError(f'negative control {negative} did not fail its intended assertion (rc={r.returncode})')
            print('negative control OK:',negative)
        elif r.returncode or 'runtime error:' in log:
            raise RuntimeError(f'positive DMA virtio test failed (rc={r.returncode})')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('--negative',choices=CONTROLS)
    p.add_argument('--positive',action='store_true');a=p.parse_args()
    if a.negative: run(a.negative)
    elif a.positive: run()
    else:
        for n in CONTROLS: run(n)
        run()
