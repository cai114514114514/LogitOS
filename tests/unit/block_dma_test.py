#!/usr/bin/env python3
"""Run verbatim NVMe/AHCI command and stop code against explicit DMA/MMIO fixtures.
This tests driver address construction and mapping lifetime, not real hardware
or the DMA API implementation. The boot storage gates provide device evidence.
"""
import argparse,os,re,subprocess
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--root',type=Path,default=Path(__file__).resolve().parents[2])
p.add_argument('--build',type=Path,required=True)
p.add_argument('--driver-source',type=Path)
a=p.parse_args();r=a.root.resolve();b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
test=Path(__file__).with_name('block_dma_test.c')
def function(s,n):
    match=re.search(r'^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;]*?\)\n\{',s,re.M)
    if not match:raise RuntimeError('missing production function '+n)
    start=match.start();end=s.index('\n}',match.end())+2
    return s[start:end]+'\n'
for driver in ('nvme','ahci'):
    src=(a.driver_source or r/'c/drivers/block')/(driver+'.c');s=src.read_text()
    structs=['nvme_sqe','nvme_cqe','nvme_q'] if driver=='nvme' else ['ahci_cmdspec','ahci_port']
    types='\n'.join(re.findall(r'^#define [A-Z][^\n]*',s,re.M))+'\n'
    for n in structs:
        match=re.search(r'^struct '+n+r' \{',s,re.M);start=match.start();end=s.index('\n}',start)+2
        end=s.index(';',end)+1;types+=s[start:end]+'\n'
    names=['nvme_begin','nvme_step','nvme_quiesce','nvme_issue','nvme_blk_poll','nvme_free_all','nvme_shutdown','nvme_remove'] if driver=='nvme' else ['ahci_dma_init','wait_clear','port_stop','port_start','port_quiesce','port_recover','ahci_unmap','ahci_issue','ahci_check','ahci_begin','ahci_step','ahci_shutdown']
    functions='\n'.join(function(s,n) for n in names)
    functions+='\n'+re.search(r'static const struct driver '+driver+r'_driver = \{.*?\n\};',s,re.S).group(0)+'\n'
    d=b/driver;d.mkdir(exist_ok=True)
    (d/'block_dma_types.inc').write_text(types)
    for negative in ('', 'old_pointer', 'old_stop') if driver=='nvme' else ('', 'old_pointer'):
        selected=functions
        if negative=='old_pointer':
            old='cmd.prp1 = addr;' if driver=='nvme' else 'uint64_t addr = dma_addr_value(segment->addr);'
            new='cmd.prp1 = (uint64_t)(uintptr_t)cpu;' if driver=='nvme' else 'uint64_t addr = (uint64_t)(uintptr_t)s->buf;'
            if selected.count(old)!=1:raise RuntimeError('old pointer control site changed')
            selected=selected.replace(old,new)
        if negative=='old_stop':
            old='if (!g_ready) {'
            if selected.count(old)!=1:raise RuntimeError('controller stop control site changed')
            selected=selected.replace(old,'if (0) {')
        name=driver+('_'+negative if negative else '')
        (d/'block_dma_functions.inc').write_text(selected)
        exe=d/name
        cmd=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-DTEST_'+driver.upper(),'-I'+str(d),'-I'+str(r/'c/drivers/core'),'-I'+str(r/'c/drivers/block'),str(test),'-o',str(exe)]
        subprocess.run(cmd,check=True)
        result=subprocess.run([str(exe)],capture_output=True,text=True)
        (d/(name+'.log')).write_text(result.stdout+result.stderr)
        expected='NVMe PRP1 is bus address, never CPU pointer' if driver=='nvme' else 'AHCI PRDT follows mapped scatter segments'
        if negative=='old_stop':expected='NVMe controller stop discards stale completion and releases pending mapping'
        if negative:
            if result.returncode!=1 or 'FAIL: '+expected not in result.stdout or 'AddressSanitizer' in result.stderr or 'runtime error:' in result.stderr:raise RuntimeError('invalid negative control '+name+': '+result.stdout+result.stderr)
            print('PASS:',name,'fails the explicit driver assertion')
        else:
            if result.returncode:raise RuntimeError(name+': '+result.stdout+result.stderr)
            print(result.stdout.strip())
    (d/'block_dma_functions.inc').write_text(functions)
