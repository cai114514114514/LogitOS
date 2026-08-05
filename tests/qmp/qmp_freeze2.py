#!/usr/bin/env python3
"""User repro v2: boot -> Studio(edit) -> CLOSE Studio -> Monitor -> CLOSE it
-> Terminal -> type. Verifies open+close cycles then terminal launch.
Usage: qmp_freeze2.py <iso> <disk> <outdir>"""
import socket, json, sys, os, time, subprocess, tempfile, shutil

iso, disk, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
work = tempfile.mktemp(suffix=".img")
shutil.copyfile(disk, work)
fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
serial = os.path.join(outdir, "freeze2-serial.log")
args = ["qemu-system-x86_64", "-cpu", "max", "-cdrom", iso,
    "-drive", f"file={work},format=raw,if=ide", "-boot", "d",
    "-m", "512", "-smp", "4", "-accel", "tcg,thread=multi", "-rtc", "base=localtime",
    "-vga", "none", "-device", "virtio-gpu-pci",
    "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
    "-serial", f"file:{serial}", "-no-reboot",
    "-qmp", f"unix:{sock},server,nowait"]
if not os.environ.get("QMP_DISPLAY"):
    args += ["-display", "none"]
proc = subprocess.Popen(args)

def armed():
    try: return "LOGIT_BOOT_OK" in open(serial, encoding="utf-8", errors="replace").read()
    except OSError: return False
for _ in range(300):
    if armed(): break
    if proc.poll() is not None: print("QEMU_DIED"); sys.exit(1)
    time.sleep(0.1)
time.sleep(2)

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
json.loads(f.readline()); cmd({"execute": "qmp_capabilities"})

cur = [640, 400]
def goto(tx, ty):
    while cur[0] != tx or cur[1] != ty:
        sx = max(-120, min(120, tx - cur[0])); sy = max(-120, min(120, ty - cur[1]))
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
        cur[0] += sx; cur[1] += sy; time.sleep(0.04)
    time.sleep(0.15)
def btn(down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": down}}]}}); time.sleep(0.1)
def click(): btn(True); btn(False)
def key(q):
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": d}}]}})
        time.sleep(0.06)
def shot(name):
    cmd({"execute": "screendump", "arguments": {"filename": os.path.join(outdir, name)}})
    time.sleep(0.4)

# cascade slots: Finder=0(110,70) Clock=1(138,98) Studio=2(166,126) Monitor=3(194,154) Terminal=4(222,182)
goto(832, 753); click(); time.sleep(3)          # open Studio
for q in "print": key(q)                        # edit
time.sleep(0.5)
goto(182, 141); click(); time.sleep(1.5)        # CLOSE Studio (close btn at x+16,y+15)
goto(512, 753); click(); time.sleep(2.5)        # open Monitor (dock idx 2)
goto(210, 169); click(); time.sleep(1.5)        # CLOSE Monitor
shot("v2-closed.ppm")
goto(576, 753); click(); time.sleep(4)          # open Terminal
for q in list("uname") + ["ret"]: key(q)
time.sleep(3)
shot("v2-terminal.ppm")
print("--- serial tail:")
try: print(open(serial, encoding="utf-8", errors="replace").read()[-1200:])
except OSError: pass
cmd({"execute": "quit"})
try: proc.wait(timeout=5)
except Exception: proc.kill()
os.unlink(work)
print("DONE")
