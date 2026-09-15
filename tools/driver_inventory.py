#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Summarize an existing LogitOS serial log; never probes or changes host devices.

An unbound registry entry is not automatically missing hardware support: legacy
IDE/framebuffer paths can operate outside that binding table. Preserve the raw
record and report boot/backend markers separately rather than inventing support.
"""
import argparse
import json
import re
from pathlib import Path

DEVICE = re.compile(r'^\[dev\] (?P<address>[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]) '
                    r'(?P<vendor>[0-9a-f]{4}):(?P<device>[0-9a-f]{4}) '
                    r'class=(?P<class>[0-9a-f]{2}\.[0-9a-f]{2}\.[0-9a-f]{2}).*? driver=(?P<driver>\S+)', re.I)
COVERAGE = {"01.01": "IDE/ATA path; check backend log", "01.06": "SATA/AHCI", "01.08": "NVMe",
            "02.00": "Ethernet", "03.00": "VGA/framebuffer", "03.02": "3D display controller",
            "04.03": "HDA audio", "0c.03": "USB host controller", "0c.05": "SMBus"}
USB_HOST = {'00': 'UHCI', '10': 'OHCI', '20': 'EHCI', '30': 'xHCI'}

def summarize(path):
    text = path.read_text(errors='replace')
    devices = {}
    for line in text.splitlines():
        m = DEVICE.match(line)
        if m:
            d = m.groupdict()
            d['bound'] = d['driver'] != '-'
            d['category'] = COVERAGE.get(d['class'][:5].lower(), 'other/chipset')
            if d['class'][:5].lower() == '0c.03':
                d['category'] = USB_HOST.get(d['class'][-2:].lower(), 'unknown USB') + ' host controller'
            d['record'] = line
            devices[d['address']] = d
    markers = [line for line in text.splitlines() if line.startswith((
        '[pci]', '[blk]', '[nvme]', '[ahci]', '[net] NIC bound:', '[irq]', '[usb]', '[uefi]', '[fb]',
        '[ehci]', '[usb-hub]', '[usb-storage]', '[usb-msc]', '[physmap]', '[widecheck] capacity',
        '[cpu-platform]', '[acpi] MADT CPUs', '[lapic]', '[smp]', '[nv-bootfb]', '[smbus]', '[spd]'))]
    usb = []
    for line in text.splitlines():
        if line.startswith('USB_DEV '):
            # Keep backend/topology fields verbatim. USB address alone is not
            # globally unique once two EHCI controllers are present.
            usb.append({'fields': dict(re.findall(r'(\w+)=([^\s]+)', line)), 'record': line})
    return {'source': str(path.resolve()), 'boot_ok': 'LOGIT_BOOT_OK' in text,
            'device_count': len(devices), 'devices': list(devices.values()),
            'usb_enumeration_records': usb,
            'backend_markers': markers,
            'note': 'Registry binding and boot markers only. This report does not prove physical operation.'}

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('serial_log', type=Path)
    p.add_argument('--json', action='store_true')
    p.add_argument('--profile', choices=['x79'], help='Attach the requested target profile; does not identify the motherboard')
    a = p.parse_args()
    result = summarize(a.serial_log)
    if a.profile == 'x79':
        result['target_profile'] = {
            'requested': 'X79 / Xeon E5 v1-v2 / 16 GiB DDR3 / GeForce GTX 1050',
            'identified_by_log': False,
            'scope': ('up to 32 logical CPUs, C600/X79 read-only DDR3 SPD, firmware boot framebuffer, '
                      'EHCI USB2 with hub topology, SATA AHCI, PCIe, and high-memory DMA'),
            'note': ('The requested profile does not identify the board revision or BIOS, actual CPUID, '
                     'DIMM population, PCI IDs, NIC, or physical GPU variant.')}
    if not result['devices']:
        p.error('no [dev] PCI records found; supply a complete LogitOS serial boot log')
    if a.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
        return
    print('LogitOS boot:', 'LOGIT_BOOT_OK observed' if result['boot_ok'] else 'completion marker absent')
    print('PCI functions:', result['device_count'])
    for d in result['devices']:
        print(f"{d['address']}  {d['vendor']}:{d['device']}  {d['class']}  {d['category']}: {d['driver']}")
    for d in result['usb_enumeration_records']:
        print(d['record'])
    if a.profile:
        print('Requested profile:', result['target_profile']['requested'], '(not identified by this log)')
    print(result['note'])

if __name__ == '__main__':
    main()
