#!/usr/bin/env python3
"""Run the existing real ring-3 HID contract with EHCI and no PS/2 device.

Only the controller/model fixture changes; every input delivery assertion is
the shared USB gate's assertion. QEMU's USB2 HID descriptors include a real
high-speed configuration. Its full-speed-only hub cannot validate TT splits.
"""
import argparse,hashlib,json,re,sys
from pathlib import Path


def verify_irq_delivery(log):
    """Require delivery in each controller's final binding generation.

    Correction: a successful sample before AML migration used to certify the
    whole boot, even if the final route delivered nothing or was LOST. Track
    evidence per controller and invalidate it on every binding rebuild. A LOST
    controller stays required, so removing its owner cannot hide its failure.
    Moved/restored routes therefore log
    their newly allocated vector; same/kept routes retain the previous one.
    EHCI_IRQ now counts dispatcher entries in the member's binding generation.
    Correction: same/kept records do not rebuild that generation, and the
    reporter emits once per generation. Retain their earlier delivery evidence;
    requiring a second report would reject a healthy unchanged route forever.
    A shared INTx dispatcher visits every member, so even this coherent count
    cannot identify the asserting device and still requires exclusive ownership.
    Correction: checking only current owners let a peer leave after increasing
    the cumulative count, falsely crediting EHCI's polling reports as IRQs.
    No count baseline is logged at that transition, so retain sharing history
    for the whole boot rather than infer which device produced the old count.
    """
    owners = {}
    shared_vectors = set()
    controllers = set()
    verified = set()
    delivered_counts = {}
    baselines = {}
    for line in log.splitlines():
        binding = re.match(r'\[irq\] (\S+): \S+ vector (\d+) \(([^)]+)\)', line)
        if binding:
            vector = int(binding[2])
            owners[binding[1]] = (vector, binding[3])
            if binding[3] == 'ehci':
                controllers.add(binding[1])
                verified.discard(binding[1])
                baselines[binding[1]] = delivered_counts.get(vector, 0)
            if sum(owner[0] == vector for owner in owners.values()) > 1:
                shared_vectors.add(vector)
                # A report already accepted on this wire is no longer proof
                # of exclusive EHCI delivery. Departure of the peer cannot
                # restore that evidence without a fresh, unshared route.
                verified.difference_update(device for device, owner in owners.items()
                                           if owner[0] == vector)
            continue

        route = re.match(r'\[irq-route\] (\S+) (.*)', line)
        if route:
            device, detail = route.groups()
            # The resolver logs disagreements before both same and moved.
            # This explains its choice; only the following outcome changes
            # binding ownership. A diagnostic alone cannot rearm the reporter.
            if detail.startswith('CONFLICT '):
                continue
            # Confirmation/fallback retention changes neither the vector nor
            # the reporter epoch. Sharing is still checked over the whole log.
            if ' same via=' in detail or ' kept via=' in detail:
                continue
            verified.discard(device)
            if 'LOST (INTx disabled)' in detail:
                owners.pop(device, None)
            elif ' moved ' in detail or ' restored' in detail:
                vector = re.search(r'\bvector (\d+)\b', detail)
                if not vector or device not in owners:
                    raise RuntimeError('reroute lacks device/vector ownership evidence: ' + line)
                owners[device] = (int(vector[1]), owners[device][1])
                if sum(owner[0] == int(vector[1]) for owner in owners.values()) > 1:
                    shared_vectors.add(int(vector[1]))
                    verified.difference_update(peer for peer, owner in owners.items()
                                               if owner[0] == int(vector[1]))
            if device in owners:
                # irq_alloc_vector() resets its counter, even if allocation
                # reuses the same number after release+bind. same/kept have
                # already retained the evidence for their unchanged epoch.
                rebuilt = ' moved ' in detail or ' restored' in detail
                baselines[device] = 0 if rebuilt else delivered_counts.get(owners[device][0], 0)
            continue

        irq = re.match(r'EHCI_IRQ vector=(\d+) delivered=([1-9]\d*) reports=([1-9]\d*)', line)
        if not irq:
            continue
        routes = [(device, owner) for device, owner in owners.items()
                  if owner[0] == int(irq[1])]
        vector = int(irq[1])
        if len(routes) != 1 or routes[0][1][1] != 'ehci' or vector in shared_vectors:
            raise RuntimeError('IRQ count is ambiguous: current ownership or shared history for vector ' + irq[1])
        device = routes[0][0]
        # There may be a probe sample and a final-route sample. The invariant
        # is one report for each controller in each route epoch, not exactly
        # two lines over an entire dual-controller boot.
        if device in verified:
            raise RuntimeError('duplicate EHCI IRQ report for the same controller route: ' + device)
        delivered = int(irq[2])
        if delivered <= baselines.get(device, 0):
            raise RuntimeError('no fresh delivery after route transition for ' + device)
        delivered_counts[vector] = delivered
        verified.add(device)
    # Recheck the final owner set too: validation is over the complete log,
    # not just the ownership visible when each EHCI_IRQ line was consumed.
    final_exclusive = {device for device, owner in owners.items()
                       if owner[1] == 'ehci' and owner[0] not in shared_vectors and
                       sum(peer[0] == owner[0] for peer in owners.values()) == 1}
    verified.intersection_update(final_exclusive)
    if not controllers or controllers - verified:
        raise RuntimeError('no final-route physical delivery for EHCI controllers: ' +
                           ', '.join(sorted(controllers - verified)))
    print('PASS EHCI IRQ: every controller delivered after its final route transition, without sharing')
    return len(controllers)


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
# QEMU creates an implicit e1000 unless disabled. It can share EHCI's probe
# vector and leave cumulative counts behind after AML moves it. This specimen
# proves exclusive EHCI delivery, so disable that unrelated NIC in BOTH modes;
# ehci_test.py separately requires rejection of the full shared-history case.
for old,new in [('"qemu-xhci,id=xhci"','"usb-ehci,id=ehci", "-nic", "none"'),
                ('"usb-kbd,id=ukbd"','"usb-kbd,id=ukbd,bus=ehci.0,usb_version=2"'),
                ('"usb-mouse,id=umouse"','"usb-mouse,id=umouse,bus=ehci.0,usb_version=2"'),
                ('mouse enumerated over xHCI','mouse enumerated over EHCI'),
                ('descriptors, delivered over MSI-X','descriptors, delivered over INTx'),
                ('"-m", "512M"','"-m", '+repr(a.memory))]:
    if code.count(old)!=1:raise RuntimeError('shared USB fixture moved: '+old)
    code=code.replace(old,new)
if a.dual:
    code=code.replace('"usb-ehci,id=ehci"','"usb-ehci,id=ehci,addr=0x5", "-device", "usb-ehci,id=ehci2,addr=0x6"')
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
    controllers = verify_irq_delivery(log)
    if a.dual and (log.count('up: ports=6 DMA32') != 2 or controllers != 2):
        raise RuntimeError('both independent EHCI instances must deliver periodic reports and real IRQs')
    print('PASS EHCI HID: real high-speed periodic transfers reached ring-3 app')
if digest(a.iso)!=iso_hash:raise RuntimeError('ISO changed during verification')
(a.out/'result.json').write_text(json.dumps({'pass':True,'negative':a.negative,'memory':a.memory,'controllers':2 if a.dual else 1,'iso_sha256':iso_hash},indent=2))
