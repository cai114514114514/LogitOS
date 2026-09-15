#!/usr/bin/env python3
"""Real guest USB input, PS/2 physically absent from the emulated machine.

USB-IF HID 1.11 section 6.2.2.5 defines absolute vs relative axes. QEMU's
usb-tablet supplies an actual report descriptor and interrupt transfers;
events.as prints events after xHCI DMA/IRQ, HID decoding and window routing.
This validates QEMU hardware emulation, not an untested physical laptop.
"""
import json
import os
from pathlib import Path
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/qmp"))
from owned_process import stop_owned


def main():
    iso, disk = map(lambda p: Path(p).resolve(), sys.argv[1:3])
    hub = os.environ.get("USB_HUB_TEST") == "1"
    artifact = iso.parent / ("usb-hub-evidence" if hub else "usb-tablet-evidence")
    artifact.mkdir(exist_ok=True)
    log = ""
    proc = ser = qmp = qfile = None

    def terminate(signum, frame):
        raise SystemExit(128 + signum)

    previous = signal.signal(signal.SIGTERM, terminate)
    with tempfile.TemporaryDirectory(prefix="logit-usb-tablet-") as tmp:
        spath, qpath = str(Path(tmp)/"serial"), str(Path(tmp)/"qmp")
        with (artifact/"qemu.err").open("w+") as err:
            try:
                cmd = [os.environ.get("QEMU", "qemu-system-x86_64"),
                       "-machine", "pc,i8042=off", "-cpu", "max", "-m", "512M",
                       "-smp", "4", "-accel", "tcg,thread=multi", "-cdrom", str(iso),
                       "-drive", f"file={disk},format=raw,if=none,id=hd0,file.locking=off",
                       "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
                       "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
                       "-device", "qemu-xhci,id=xhci", "-device", "usb-kbd,id=kbd",
                       "-device", "usb-tablet,id=tablet", "-display", "none", "-no-reboot",
                       "-serial", f"unix:{spath},server=on,wait=off",
                       "-qmp", f"unix:{qpath},server=on,wait=off"]
                if hub:
                    # QEMU's hub is USB1.1/full-speed. This tests real route
                    # strings and downstream input, not HS transaction translation.
                    i=cmd.index("usb-kbd,id=kbd")
                    cmd[i]="usb-kbd,id=kbd,bus=xhci.0,port=1.1"
                    i=cmd.index("usb-tablet,id=tablet")
                    cmd[i]="usb-tablet,id=tablet,bus=xhci.0,port=1.2"
                    cmd += ["-device","usb-hub,id=hub,bus=xhci.0,port=1"]
                    # QEMU creates command-line devices in order: the hub must
                    # claim port 1 before its child paths are attached.
                    cmd[-2:]=[]
                    i=cmd.index("qemu-xhci,id=xhci")+1
                    cmd[i:i]=["-device","usb-hub,id=hub,bus=xhci.0,port=1"]
                (artifact/"command.json").write_text(json.dumps(cmd,indent=2)+"\n")
                proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)

                def connect(path):
                    deadline = time.monotonic()+20
                    while time.monotonic()<deadline:
                        s=socket.socket(socket.AF_UNIX)
                        try:
                            s.connect(path); return s
                        except OSError:
                            s.close()
                            if proc.poll() is not None: raise RuntimeError("QEMU exited before sockets")
                            time.sleep(.05)
                    raise RuntimeError("QEMU socket timeout")

                ser, qmp = connect(spath), connect(qpath)
                ser.setblocking(False); qmp.settimeout(10)
                qfile=qmp.makefile("rw")

                def pump(seconds):
                    nonlocal log
                    deadline=time.monotonic()+seconds
                    while time.monotonic()<deadline:
                        try:
                            data=ser.recv(65536)
                            if data:
                                log+=data.decode("utf8","replace"); continue
                        except BlockingIOError: pass
                        if proc.poll() is not None: raise RuntimeError("QEMU exited")
                        time.sleep(.02)

                def wait(marker, timeout):
                    deadline=time.monotonic()+timeout
                    while marker not in log and time.monotonic()<deadline: pump(.2)
                    if marker not in log: raise RuntimeError("missing guest marker: "+marker)

                def qcmd(name,args=None):
                    d={"execute":name}
                    if args is not None:d["arguments"]=args
                    qfile.write(json.dumps(d)+"\n");qfile.flush()
                    while True:
                        line=qfile.readline()
                        if not line:raise RuntimeError("QMP closed")
                        reply=json.loads(line)
                        if "error" in reply:raise RuntimeError(str(reply))
                        if "return" in reply:return reply["return"]

                def send(*events): qcmd("input-send-event",{"events":list(events)})
                def key(k,down):return {"type":"key","data":{"down":down,"key":{"type":"qcode","data":k}}}
                def button(k,down):return {"type":"btn","data":{"button":k,"down":down}}
                def point(x,y):
                    send({"type":"abs","data":{"axis":"x","value":round(x*32767/1279)}},
                         {"type":"abs","data":{"axis":"y","value":round(y*32767/799)}})
                    pump(.4)
                def events(mark):
                    return [tuple(map(int,m.groups())) for m in re.finditer(
                        r"^EV (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+) (-?\d+)\s*$",log[mark:],re.M)]
                def require(ok,what):
                    if not ok:raise RuntimeError(what)

                qcmd("qmp_capabilities")
                wait("LOGIT_BOOT_OK",180); pump(6)
                if hub:
                    require(re.search(r"USB_HUB .*children=2",log),"hub did not enumerate both downstream input devices")
                require("role=keyboard decode=report-descriptor" in log,"USB keyboard did not bind")
                require("role=mouse decode=report-descriptor" in log,"USB tablet did not bind")
                ser.sendall(b"as /usr/as/examples/events.as &\n")
                wait("EVENTS-READY",90); pump(1)
                first=len(log);point(650,420)
                a=[e for e in events(first) if e[0]==7]
                require(a,"first absolute pointer sample did not reach app")
                second=len(log);point(760,470)
                b=[e for e in events(second) if e[0]==7]
                require(b,"second absolute pointer sample did not reach app")
                require(abs((b[-1][1]-a[-1][1])-110)<=1 and abs((b[-1][2]-a[-1][2])-50)<=1,
                        f"tablet did not preserve absolute displacement: {a[-1]} -> {b[-1]}")

                mark=len(log)
                send(key("shift",True));pump(.15)
                send(key("a",True));pump(.1);send(key("a",False));pump(.1)
                send(button("left",True));pump(.2);send(button("left",False));pump(.2)
                send(key("shift",False));pump(.15)
                send(key("a",True));pump(.1);send(key("a",False));pump(.3)
                ev=events(mark)
                require(sum(e[0]==1 and e[1]==65 and e[3]&1 for e in ev)==1,"shifted A with USB modifier must arrive exactly once")
                require(sum(e[0]==1 and e[1]==97 and e[3]==0 for e in ev)==1,"released Shift must produce exactly one unmodified a")
                clicks=[e for e in ev if e[0]==2 and e[4]==1]
                releases=[e for e in ev if e[0]==6 and e[4]==1]
                require(clicks and releases,"tablet button press/release missing")
                require(clicks[-1][3]&1,"USB Shift missing from tablet click")
                require(clicks[-1][1:3]==b[-1][1:3] and releases[-1][1:3]==b[-1][1:3],
                        "repeated absolute button reports drifted from last pointer position")
                delta=(b[-1][1]-a[-1][1],b[-1][2]-a[-1][2])
                print(f"PASS guest: USB keyboard + absolute tablet, PS/2 absent; observed displacement {delta} (target 110,50; tolerance 1 pixel), stable click/release coordinates, shifted A/click and released-Shift a")
                print("Evidence:",artifact)
                return 0
            except (RuntimeError,OSError) as exc:
                print("FAIL:",exc);print(log[-5000:]);return 1
            finally:
                (artifact/"serial.log").write_text(log)
                for stream in (qfile,qmp,ser):
                    if stream:
                        try:stream.close()
                        except OSError:pass
                if proc is not None:stop_owned(proc)
                signal.signal(signal.SIGTERM,previous)


if __name__=="__main__":sys.exit(main())
