#!/usr/bin/env python3
"""Real xHCI TU and rings using the existing non-identity DMA hardware leaves."""
from pathlib import Path
import argparse,os,platform,re,subprocess
ap=argparse.ArgumentParser();ap.add_argument('--build',type=Path,required=True);ap.add_argument('--negative-only',action='store_true');a=ap.parse_args()
r=Path(__file__).resolve().parents[2]
variants=[('',None)] if not a.negative_only else [('residual','short packet stops bulk at its actual byte count'),('accounting','control status completion retires the entire TD over repeated ring wraps'),('dequeue','STALL recovery advances dequeue past the failed control TD'),('toggle','healthy endpoint clear-halt resets DATA0 with drop/add context')]
for name,marker in variants:
    b=a.build.resolve()/(name or 'positive');b.mkdir(parents=True,exist_ok=True)
    s=(r/'c/drivers/usb/xhci.c').read_text()
    # This host model owns the register completions and ordering assertions.
    # Preserve the full C driver while spelling its x86 mfence as a portable
    # sequentially consistent fence, so the Apple Silicon host runs it natively.
    s=s.replace('__asm__ volatile ("mfence" ::: "memory")',
                '__atomic_thread_fence(__ATOMIC_SEQ_CST)')
    if name=='residual':s=s.replace('uint32_t got=chunk-(uint32_t)residual;','uint32_t got=chunk; /* negative: ignore short residual */')
    if name=='accounting':s=s.replace('{ IO_GUARD(&xhci_events_gate); ep->ring.pending=0; }','/* negative: leak Setup/Data TRB accounting */')
    if name=='dequeue':s=s.replace('TRB_SET_TYPE(16)|target,NULL)','TRB_SET_TYPE(TRB_NOOP_CMD),NULL)')
    if name=='toggle':s=s.replace('input_ctrl_ctx(s->in_ctx)[0]=1u<<dci;','input_ctrl_ctx(s->in_ctx)[0]=0; /* negative: retain old sequence */')
    s=re.sub(r'return \*\(volatile uint32_t\s*\*\)\(([^;]+)\);',r'return test_read(\1,4);',s)
    s=re.sub(r'\*\(volatile uint32_t\s*\*\)\(([^;]+)\) = v;',r'test_write(\1,4,v);',s)
    s=s.replace('x->db[slot] = target;','test_write(&x->db[slot],4,target);')
    (b/'dma_driver.inc').write_text(s)
    fixture=(r/'tests/unit/dma_driver_test.c').read_text().replace('int main(int argc,char **argv)','int dma_baseline_main(int argc,char **argv)')
    fixture=fixture.replace('void test_write(volatile void *p, unsigned n, uint64_t v) {','static void xhci_model_write(size_t,uint64_t);\nvoid test_write(volatile void *p, unsigned n, uint64_t v) {')
    fixture=fixture.replace('size_t o=(const unsigned char*)p-test_regs;','size_t o=(const unsigned char*)p-test_regs;\n    xhci_model_write(o,v);')
    fixture+='\n'+(r/'tests/unit/xhci_xfer_cases.c').read_text()
    (b/'test.c').write_text(fixture)
    inc=['c/drivers/usb','c/drivers/core','c/kernel/pci','c/kernel/mm','c/kernel/mm/phys','c/kernel/mm/virt','c/kernel/mm/cache','c/kernel/mm/reclaim','c/kernel/core','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/init','c/kernel/diag','c/kernel/sync','c/kernel/cpu','c/kernel/cpu/acpi','c/kernel/cpu/irq','c/kernel/cpu/smp','c/kernel/cpu/acpi','c/kernel/cpu/irq','c/kernel/cpu/smp','c/kernel/sched','c/drivers/timer','include']
    cmd=[os.environ.get('CC','clang'),'-O1','-g','-pthread','-DDRIVER_KIND=4','-DLOGIT_HOST_TEST','-ffunction-sections','-fdata-sections','-fsanitize=address,undefined','-I'+str(b),'-Itests/unit/dma_driver_stub']+['-I'+p for p in inc]+[str(b/'test.c'),'c/drivers/usb/xhci_ring.c','-o',str(b/'test')]
    cmd+=['-Wl,-dead_strip'] if platform.system()=='Darwin' else ['-Wl,--gc-sections']
    subprocess.run(cmd,cwd=r,check=True)
    p=subprocess.run([str(b/'test')],capture_output=True,text=True,timeout=30);(b/'result.log').write_text(p.stdout+p.stderr)
    if marker:assert p.returncode==1 and 'FAIL '+marker in p.stdout,(p.stdout,p.stderr)
    else:assert p.returncode==0,(p.stdout,p.stderr)
    print((name or 'positive')+': '+p.stdout.strip().splitlines()[-1])
