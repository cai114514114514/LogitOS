#!/usr/bin/env python3
"""GUI Terminal smoke test, driven over QMP.

Two defects it used to have, both of which made it a test that could not fail:

  1. IT COMPUTED A VERDICT AND THREW IT AWAY. It printed PASS or FAIL and then
     exited 0 either way, so `make` was green whatever the machine did. It now
     exits non-zero on any failed check.

  2. ITS ONE ASSERTION COULD NOT REACH WHAT IT READ. QEMU runs with -snapshot,
     so guest writes go to a temporary overlay and never touch disk.img -- and
     the check was `grep the host's disk.img for the text we just wrote`. That
     is false by construction: with -snapshot it can only ever fail, and
     without -snapshot it would corrupt the image every other run (see the
     logitfs cross-boot durability note in CLAUDE.md).

     The fix asks the GUEST. The GUI Terminal writes the file; the SERIAL
     console's shell -- a second, independent /bin/sh inside the same machine,
     with the same filesystem and no rich channel -- reads it back. The bytes
     that come out of the serial port are the guest's own answer, and they work
     under -snapshot because nothing has to reach the host at all.

Dock geometry (9 icons @1280x800): icon i center = (384 + i*64, 753);
terminal is index 3 -> (576,753). Cursor starts at screen center (640,400).
"""
import json
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

iso, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)  # QEMU binds it
ser = tempfile.mktemp(suffix=".ser")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen([qemu, "-cpu", "max", "-cdrom", iso,
    "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk", "-boot", "d",
    "-snapshot",            # ephemeral writes -- so the readback must be in-guest
    "-display", "none", "-no-reboot", "-m", "512M",
    "-chardev", f"socket,id=ser0,path={ser},server=on,wait=on",
    "-serial", "chardev:ser0",
    "-qmp", f"unix:{sock},server,nowait"])

# The serial console is both the boot log and the second shell. It is drained by
# a thread from the moment it exists: the chardev socket has a finite buffer and
# a host that stops reading it blocks the guest inside serial_puts.
serial = socket.socket(socket.AF_UNIX)
for _ in range(300):
    try:
        serial.connect(ser); break
    except OSError:
        if proc.poll() is not None:
            print("qemu died before the serial socket appeared"); sys.exit(1)
        time.sleep(0.1)
log = bytearray()
stop = False


def reader():
    while not stop:
        try:
            b = serial.recv(65536)
            if not b:
                break
            log.extend(b)
        except OSError:
            time.sleep(0.05)


threading.Thread(target=reader, daemon=True).start()

deadline = time.time() + 90
while b"LOGIT_BOOT_OK" not in log and time.time() < deadline:
    if proc.poll() is not None:
        print("qemu died before boot"); sys.exit(1)
    time.sleep(0.1)
if b"LOGIT_BOOT_OK" not in log:
    print("FAIL: never booted"); proc.kill(); sys.exit(1)
time.sleep(2.0)

s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try: s.connect(sock); break
    except OSError: time.sleep(0.1)
f = s.makefile("rw")
def recv():
    while True:
        line = f.readline()
        if not line: return None
        m = json.loads(line)
        if "return" in m or "error" in m: return m
def cmd(d): f.write(json.dumps(d)+"\n"); f.flush(); return recv()
cur=[640,400]
def goto(tx,ty):
    while cur[0]!=tx or cur[1]!=ty:
        sx=max(-120,min(120,tx-cur[0])); sy=max(-120,min(120,ty-cur[1]))
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"rel","data":{"axis":"x","value":sx}},{"type":"rel","data":{"axis":"y","value":sy}}]}})
        cur[0]+=sx; cur[1]+=sy; time.sleep(0.05)
    time.sleep(0.15)
def click():
    for d in (True,False):
        cmd({"execute":"input-send-event","arguments":{"events":[{"type":"btn","data":{"button":"left","down":d}}]}}); time.sleep(0.12)
def key(q):
    for d in (True,False):
        cmd({"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":q},"down":d}}]}})
    time.sleep(0.18)
def shift_key(q):
    cmd({"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"shift"},"down":True}}]}}); time.sleep(0.04)
    key(q)
    cmd({"execute":"input-send-event","arguments":{"events":[{"type":"key","data":{"key":{"type":"qcode","data":"shift"},"down":False}}]}}); time.sleep(0.04)
KMAP={" ":"spc",".":"dot","\n":"ret","-":"minus","/":"slash"}
def send(t):
    for ch in t:
        if ch==">": shift_key("dot")
        elif ch=="|": shift_key("backslash")
        else: key(KMAP.get(ch,ch))

fails = []
def chk(cond, msg):
    print(("  ok   " if cond else "  FAIL ") + msg)
    if not cond:
        fails.append(msg)

json.loads(f.readline()); cmd({"execute":"qmp_capabilities"})
goto(576,753); click(); time.sleep(1.5)        # launch Terminal from the Dock (icon 3 of 9)
for line in ["uname\n","ls /bin | wc\n","echo logit-os-is-real > /hi.txt\n"]:
    send(line); time.sleep(2.0)
time.sleep(1.5)
cmd({"execute":"screendump","arguments":{"filename":out}}); time.sleep(0.5)

# Ask the guest. The GUI Terminal wrote /hi.txt through its own /bin/sh; this is
# a different shell, on a different console, reading the same filesystem.
MARK = b"---TERMTEST---"
start = len(log)
serial.sendall(b"echo " + MARK + b"\ncat /hi.txt\necho " + MARK + b"\n")
deadline = time.time() + 25
while log.count(MARK, start) < 3 and time.time() < deadline:
    time.sleep(0.2)
body = bytes(log[start:])
a = body.find(MARK)
b1 = body.find(MARK, a + len(MARK)) if a >= 0 else -1
c = body.find(MARK, b1 + len(MARK)) if b1 >= 0 else -1
between = body[b1 + len(MARK):c] if (b1 >= 0 and c > b1) else b""

chk(b"logit-os-is-real" in between,
    "the file the GUI Terminal wrote is readable from the serial console's shell")
chk(b"LRT\x01" not in body,
    "and the non-rich shell's output carries no protocol bytes")

stop = True
try: cmd({"execute":"quit"})
except Exception: pass
try: proc.wait(timeout=5)
except Exception: proc.kill()
for p in (sock, ser):
    try: os.unlink(p)
    except OSError: pass

if fails:
    print("FAIL (%d):" % len(fails))
    for m in fails:
        print("   " + m)
    sys.exit(1)
print("PASS")
