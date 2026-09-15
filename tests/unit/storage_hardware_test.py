#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Compile real storage discovery/identify/handoff functions, plus watched controls."""
import argparse
import os
from pathlib import Path
import re
import subprocess

ap=argparse.ArgumentParser(description=__doc__)
ap.add_argument('--build', required=True, type=Path)
ap.add_argument('--controls-only',action='store_true')
ap.add_argument('--positive-only',action='store_true')
a=ap.parse_args()
r=Path(__file__).resolve().parents[2];b=a.build.resolve();b.mkdir(parents=True,exist_ok=True)
nvme=(r/'c/drivers/block/nvme.c').read_text();ahci=(r/'c/drivers/block/ahci.c').read_text()
def function(s,n):
    match=re.search(r'^(?:static )?(?:inline )?[\w *]+\b'+n+r'\([^;]*?\)\n\{',s,re.M)
    if not match:raise RuntimeError('missing production function '+n)
    end=s.index('\n}',match.end())+2
    return s[match.start():end]+'\n'
selected='\n'.join(function(nvme,n) for n in ['nvme_find','nvme_controller_ioqs','nvme_granted_ioqs','nvme_namespace_format'])
selected+='\n'+'\n'.join(function(ahci,n) for n in [
    'ahci_hba_key','ahci_hba_is_quarantined','ahci_hba_quarantine',
    'ahci_pci_command_read','ahci_pci_command_write',
    'ahci_pci_restore_initial','ahci_pci_mem_enable','ahci_find',
    'ahci_identify_sector_bytes','port_identify','ahci_bios_handoff',
    'ahci_pci_master_quiet','ahci_pci_master_enable',
    'ahci_stop_firmware_ports','ahci_prepare_controller'])
# This fixture must keep the production callers as well as helper definitions:
# otherwise an orphan helper could make the gate green while the driver ignores it.
init=function(nvme,'nvme_init');bringup=function(ahci,'ahci_bring_up')
find=function(ahci,'ahci_find')
assert 'nvme_find()' in init and 'nvme_namespace_format(idbuf, &g_cap, &g_lba)' in init
assert 'nvme_controller_ioqs(cap, dev->res[0].size)' in init
assert 'nvme_granted_ioqs(g_admin.result, wanted_ioqs)' in init
assert init.index('cmd.cdw10 = 0x07') < init.index('for (unsigned i = 0; i < wanted_ioqs;')
assert bringup.index('ahci_prepare_controller(dev, abar') < bringup.index('ahci_dma_init(p, cap)')
prepare=function(ahci,'ahci_prepare_controller')
assert prepare.index('ahci_bios_handoff(abar)') < prepare.index('ahci_pci_master_quiet(h)')
assert prepare.index('ahci_pci_master_quiet(h)') < prepare.index('ahci_stop_firmware_ports(abar, cap, pi)')
assert prepare.index('ahci_stop_firmware_ports(abar, cap, pi)') < prepare.index('ahci_pci_master_enable(h)')
assert find.index('ahci_pci_mem_enable(out)') < find.index('dev_bar_map(d, 5)')
assert 'if (!dev.abar)' in function(ahci,'ahci_init')
structs='\n'.join([
    re.search(r'struct ahci_cmdspec \{.*?\n\};',ahci,re.S).group(0),
    re.search(r'struct ahci_hba \{.*?\n\};',ahci,re.S).group(0),
])
defines='\n'.join(re.findall(r'^#define (?:AQ_DEPTH|NVME_IOQ_MAX|AHCI_[A-Z0-9_]+|HBA_[A-Z0-9_]+|GHC_[A-Z0-9_]+|CAP_[A-Z0-9_]+|BOHC_[A-Z0-9_]+|P_[A-Z0-9_]+|CMD_[A-Z0-9_]+|SPIN_INIT|ATA_IDENTIFY)\s+.*',nvme+'\n'+ahci,re.M))
(b/'storage_hardware_types.inc').write_text(
    defines+'\n'+structs+'\n'
    +'static uint32_t g_ahci_quarantine[AHCI_MAX_HBA];\n'
    +'static unsigned g_ahci_quarantine_count;\n')
controls=[('old_nvme_queue_grant','return granted < requested ? granted : requested;','(void)granted; return requested;',
           ('NVMe honors controller SQ queue grant','NVMe honors controller CQ queue grant')),
          ('old_nvme_scan','&& !dev->drv)','&& !dev->drv && dev->bus == 0 && dev->func == 0)','NVMe finds bridge-bus multifunction endpoint'),
          ('old_ahci_mem_command_readback','int decode_ok = (after & PCI_CMD_MEM) != 0;','int decode_ok = 1;','AHCI refuses MEM-decode drop before BAR mapping'),
          ('old_ahci_master_command_readback','int master_ok = (after & PCI_CMD_MASTER) != 0;','int master_ok = 1;','AHCI refuses bus-master drop after quiescing firmware ports'),
          ('old_ahci_restore_failure_continue','panic("ahci: PCI Command restore unconfirmed");','return;',
           'AHCI fail-stops when partial Command enable cannot be restored'),
          ('old_ahci_early_bme',
           'uint16_t wanted = (uint16_t)(old | PCI_CMD_MEM);\n'
           '    ahci_pci_command_write(h, wanted);\n'
           '    uint16_t after = ahci_pci_command_read(h);\n'
           '    int decode_ok = (after & PCI_CMD_MEM) != 0;\n'
           '    int master_unchanged = ((after ^ old) & PCI_CMD_MASTER) == 0;',
           'uint16_t wanted = (uint16_t)(old | PCI_CMD_MEM | PCI_CMD_MASTER);\n'
           '    ahci_pci_command_write(h, wanted);\n'
           '    uint16_t after = ahci_pci_command_read(h);\n'
           '    int decode_ok = (after & PCI_CMD_MEM) != 0;\n'
           '    int master_unchanged = 1;',
           'AHCI enables BME only after BOHC and firmware-port stop'),
          ('old_ahci_skip_firmware_stop',
           'if (ahci_stop_firmware_ports(abar, cap, pi)) {',
           'if (0) {',
           'AHCI enables BME only after BOHC and firmware-port stop'),
          ('old_ahci_bme_isolation_continue',
           'panic("ahci: BME quarantine failed");',
           'return -1;',
           'AHCI fail-stops if failed BME enable cannot be isolated'),
          ('old_ahci_firmware_bme_disable_continue',
           'panic("ahci: BME disable unconfirmed after BIOS handoff");',
           'return;',
           'AHCI fail-stops if firmware BME cannot be disabled after handoff'),
          ('old_ahci_sectors','if (sector_bytes != 512) {','if (0) {',
           ('AHCI rejects 4Kn before registering a disk','AHCI rejects invalid zero logical-sector size')),
          ('old_ahci_handoff','if (wait_clear(abar, HBA_BOHC, BOHC_BOS | BOHC_BB, SPIN_INIT, 2000)) {','if (0) {',
           ('AHCI enables BME only after BOHC and firmware-port stop',
            'AHCI refuses controller while firmware retains ownership'))]
variants=[] if a.controls_only else [('positive',None,None,None)]
if not a.positive_only:variants+=controls
for name,old,new,expected in variants:
    code=selected
    if old:
        if code.count(old)!=1:raise RuntimeError('control site changed '+name)
        code=code.replace(old,new)
    (b/'storage_hardware_functions.inc').write_text(code)
    exe=b/name
    cmd=[os.environ.get('CC','cc'),'-std=c11','-O1','-g','-Wall','-Wextra','-Wno-unused-function','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(b),'-I'+str(r/'c/drivers/core'),'-I'+str(r/'c/kernel/pci'),str(r/'tests/unit/storage_hardware_test.c'),'-o',str(exe)]
    subprocess.run(cmd,check=True)
    result=subprocess.run([str(exe)],capture_output=True,text=True)
    (b/(name+'.log')).write_text(result.stdout+result.stderr)
    if expected:
        failed=[line for line in result.stdout.splitlines() if line.startswith('FAIL: ')]
        labels=list(expected) if isinstance(expected,tuple) else [expected]
        wanted=['FAIL: '+label for label in labels]
        if (result.returncode!=1 or failed!=wanted or result.stderr or
                not re.search(r'^storage hardware: \d+ checks, '+str(len(labels))+
                              r' failed$',result.stdout,re.M)):
            raise RuntimeError('control failed for wrong reason: '+name+'\n'+result.stdout+result.stderr)
        print('EXPECTED-FAIL '+name+': '+'; '.join(labels))
    elif result.returncode:
        raise RuntimeError(result.stdout+result.stderr)
    else:print(result.stdout.strip())
(b/'storage_hardware_functions.inc').write_text(selected)
