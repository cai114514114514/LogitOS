#!/usr/bin/env python3
"""User's exact repro: boot with `make run` flags, open Code Studio, edit,
then open Terminal. Screendump diffs detect a frozen WM (0 diff = frozen).
Usage: qmp_freeze.py <iso> <disk> <outdir>"""
import socket, json, sys, os, time, subprocess, tempfile, shutil

iso, disk, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
work = tempfile.mktemp(suffix=".img")
shutil.copyfile(disk, work)          # private writable copy (no lock clash, no pollution)
fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
serial = os.path.join(outdir, "freeze-serial.log")
args = ["qemu-system-x86_64", "-cdrom", iso,
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
    try: return "AETHER_BOOT_OK" in open(serial).read()
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
    p = os.path.join(outdir, name)
    cmd({"execute": "screendump", "arguments": {"filename": p}}); time.sleep(0.4)
    return p

# 1) open Code Studio (dock idx 7) and "edit" (mash keys into it)
goto(832, 753); click(); time.sleep(3)
for q in "print": key(q)
time.sleep(1)
shot("step1-studio.ppm")

# 2) open Terminal (dock idx 3)
goto(576, 753); click(); time.sleep(4)
p1 = shot("step2-terminal.ppm")

# 3) type a command into the terminal
for q in list("uname") + ["ret"]: key(q)
time.sleep(3)
p2 = shot("step3-typed.ppm")

# 4) is the WM alive? drag the terminal window by its titlebar
goto(cur[0] - 100, 108); btn(True); goto(cur[0] - 300, 200); btn(False)
time.sleep(1.5)
p3 = shot("step4-dragged.ppm")

def diff(a, b):
    da, db = open(a, "rb").read(), open(b, "rb").read()
    n = min(len(da), len(db))
    return sum(1 for i in range(0, n, 997) if da[i] != db[i])   # sampled

print("DIFF term-open->typed:", diff(p1, p2))
print("DIFF typed->dragged:", diff(p2, p3))
print("--- serial tail:")
try: print(open(serial).read()[-1500:])
except OSError: pass
cmd({"execute": "quit"})
try: proc.wait(timeout=5)
except Exception: proc.kill()
os.unlink(work)
print("DONE")
