#!/usr/bin/env python3
"""AetherFS v3 UI smoke test, self-contained and reproducible.

Boots the ISO headless with a QMP socket, waits until the kernel is armed
(serial prints AETHER_BOOT_OK -- injecting input before mouse_init() silently
drops events), drives the Terminal over QMP to create a nested file, screenshots,
then inspects the raw disk image to confirm the write persisted.

Usage: qmp_fs.py <aether.iso> <disk.img> [out.ppm]

Notes baked in from debugging this stack:
  - wait for AETHER_BOOT_OK before sending any input;
  - QEMU qcodes are 'ctrl'/'shift' (NOT 'ctrl_l'/'shift_l');
  - PS/2 relative motion is clamped to ~9 bits, so step moves <=200 px.
"""
import socket, json, sys, os, time, subprocess, tempfile

iso, disk = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else "build/fs_smoke.ppm"
sock = tempfile.mktemp(suffix=".qmp")
serial = tempfile.mktemp(suffix=".log")
qemu = os.environ.get("QEMU", "qemu-system-x86_64")

proc = subprocess.Popen([
    qemu, "-cdrom", iso,
    "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk", "-boot", "d",
    "-display", "none", "-no-reboot",
    "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server,nowait",
])

def armed():
    try:
        with open(serial) as fh:
            return "AETHER_BOOT_OK" in fh.read()
    except OSError:
        return False

def fail(msg):
    print("FAIL:", msg)
    try: proc.kill()
    except Exception: pass
    sys.exit(1)

for _ in range(200):                       # wait for the kernel to arm input
    if armed(): break
    if proc.poll() is not None: fail("qemu exited during boot")
    time.sleep(0.1)
else:
    fail("AETHER_BOOT_OK never appeared")
time.sleep(0.4)

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
def cmd(d): f.write(json.dumps(d) + "\n"); f.flush(); return recv()

cur = [512, 384]
def goto(tx, ty):
    while cur[0] != tx or cur[1] != ty:
        sx = max(-200, min(200, tx - cur[0])); sy = max(-200, min(200, ty - cur[1]))
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
        cur[0] += sx; cur[1] += sy; time.sleep(0.06)
    time.sleep(0.12)
def click():
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": d}}]}}); time.sleep(0.1)
def key(q):
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": d}}]}})
    time.sleep(0.05)
def shift_key(q):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": True}}]}}); time.sleep(0.05)
    key(q)
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "key", "data": {"key": {"type": "qcode", "data": "shift"}, "down": False}}]}}); time.sleep(0.05)
KMAP = {" ": "spc", ".": "dot", "\n": "ret", "-": "minus"}
def send(t):
    for ch in t:
        if ch == ">": shift_key("dot")
        else: key(KMAP.get(ch, ch))

json.loads(f.readline()); cmd({"execute": "qmp_capabilities"})
goto(607, 720); click(); time.sleep(1.0)            # launch Terminal from the Dock
for line in ["mkdir proj\n", "cd proj\n", "echo smokeprobe > note.txt\n", "ls\n", "cat note.txt\n"]:
    send(line); time.sleep(0.5)
time.sleep(0.4)
cmd({"execute": "screendump", "arguments": {"filename": out}}); time.sleep(0.4)
cmd({"execute": "quit"})
try: proc.wait(timeout=5)
except Exception: proc.kill()

with open(disk, "rb") as fh:
    blob = fh.read()
ok = b"smokeprobe" in blob and b"proj" in blob and b"note.txt" in blob
os.unlink(sock) if os.path.exists(sock) else None
os.unlink(serial) if os.path.exists(serial) else None
print("PASS: /proj/note.txt created and persisted" if ok else "FAIL: probe not found on disk")
sys.exit(0 if ok else 1)
