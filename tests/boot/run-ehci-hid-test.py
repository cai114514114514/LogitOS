#!/usr/bin/env python3
"""Run the existing real ring-3 HID contract with EHCI and no PS/2 device.

Only the controller/model fixture changes; every input delivery assertion is
the shared USB gate's assertion. QEMU's USB2 HID descriptors include a real
high-speed configuration. Its full-speed-only hub cannot validate TT splits.
"""
import argparse,hashlib,json,re,sys
from pathlib import Path
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--iso',required=True,type=Path)
p.add_argument('--disk',required=True,type=Path)
p.add_argument('--out',required=True,type=Path)
p.add_argument('--memory',default='512M')
p.add_argument('--dual',action='store_true',help='keyboard and mouse on separate EHCI controllers')
p.add_argument('--negative',action='store_true')
a=p.parse_args();a.out=a.out.resolve();a.out.mkdir(parents=True,exist_ok=True)
def digest(path):
    h=hashlib.sha256()
    with path.open('rb') as f:
        for chunk in iter(lambda:f.read(1048576),b''):h.update(chunk)
    return h.hexdigest()
iso_hash=digest(a.iso)
source=Path(__file__).resolve().parents[1]/'qmp/qmp_usb.py'
code=source.read_text()
for old,new in [('"qemu-xhci,id=xhci"','"usb-ehci,id=ehci"'),
                ('"usb-kbd,id=ukbd"','"usb-kbd,id=ukbd,bus=ehci.0,usb_version=2"'),
                ('"usb-mouse,id=umouse"','"usb-mouse,id=umouse,bus=ehci.0,usb_version=2"'),
                ('mouse enumerated over xHCI','mouse enumerated over EHCI'),
                ('descriptors, delivered over MSI-X','descriptors, delivered over INTx'),
                ('"-m", "512M"','"-m", '+repr(a.memory))]:
    if code.count(old)!=1:raise RuntimeError('shared USB fixture moved: '+old)
    code=code.replace(old,new)
if a.dual:
    code=code.replace('"usb-ehci,id=ehci"','"usb-ehci,id=ehci,addr=0x5", "-device", "usb-ehci,id=ehci2,addr=0x6", "-nic", "none"')
    code=code.replace('usb-mouse,id=umouse,bus=ehci.0','usb-mouse,id=umouse,bus=ehci2.0')
    old='m = re.search(r"USB_READY devices=(\\d+) drivers=(\\d+)", log)'
    if code.count(old)!=1:raise RuntimeError('shared USB ready parser moved')
    code=code.replace(old,'m = list(re.finditer(r"USB_READY devices=(\\d+) drivers=(\\d+)", log))[-1]')
sys.argv=[str(source),str(a.iso.resolve()),str(a.disk.resolve())]+(['--no-devices'] if a.negative else [])
ns={'__name__':'__main__','__file__':str(source)}
rc=0
try:exec(compile(code,str(source),'exec'),ns)
except SystemExit as exc:rc=exc.code or 0
finally:
    log=ns.get('log','')
    (a.out/'serial.log').write_text(log)
    (a.out/'command.json').write_text(json.dumps(ns.get('cmd'),indent=2))
    if 'tmp' in ns:
        stderr=Path(ns['tmp'])/'qemu.err'
        if stderr.exists():(a.out/'qemu.log').write_bytes(stderr.read_bytes())
if a.negative:
    if rc!=2 or 'LOGIT_BOOT_OK' not in log or 'USB_READY devices=0' not in log:
        raise RuntimeError('EHCI no-device negative failed for an unrelated reason')
    print('EXPECTED-FAIL EHCI HID: no USB input devices cannot deliver app events')
else:
    if rc or '[ehci]' not in log or log.count('speed=high')<2:
        raise RuntimeError('EHCI HID contract did not pass with two high-speed devices')
    irqs=list(re.finditer(r'EHCI_IRQ vector=(\d+) delivered=([1-9]\d*) reports=([1-9]\d*)',log))
    if not irqs:raise RuntimeError('no physical vector delivery recorded for EHCI HID')
    for irq in irqs:
        routes=re.findall(r'\[irq\] ([^:]+:[^:]+:[^ ]+): intx vector '+irq[1]+r' \(([^)]+)\)',log)
        if len(routes)!=1 or routes[0][1]!='ehci':
            raise RuntimeError('IRQ count is ambiguous: another registered device shares the EHCI test vector')
    if a.dual and (log.count('up: ports=6 DMA32')!=2 or len(re.findall(r'EHCI_IRQ vector=',log))!=2):
        raise RuntimeError('both independent EHCI instances must deliver periodic reports and real IRQs')
    print('PASS EHCI HID: real high-speed periodic transfers reached ring-3 app')
if digest(a.iso)!=iso_hash:raise RuntimeError('ISO changed during verification')
(a.out/'result.json').write_text(json.dumps({'pass':True,'negative':a.negative,'memory':a.memory,'controllers':2 if a.dual else 1,'iso_sha256':iso_hash},indent=2))
