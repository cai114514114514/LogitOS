#!/usr/bin/env python3
# Dock geometry (9 icons @1280x800): icon i center = (384 + i*64, 753);
# terminal is index 3 -> (576,753). Cursor starts at screen center (640,400).
import socket, json, sys, os, time, subprocess, tempfile
iso, disk, out = sys.argv[1], sys.argv[2], sys.argv[3]
fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)  # QEMU binds the socket itself
fd, serial = tempfile.mkstemp(suffix=".log"); os.close(fd)
qemu = os.environ.get("QEMU", "qemu-system-x86_64")
proc = subprocess.Popen([qemu, "-cdrom", iso,
    "-drive", f"file={disk},format=raw,if=ide,index=0,media=disk", "-boot", "d",
    "-snapshot",            # ephemeral writes -> the disk probe can't leak across runs
    "-display", "none", "-no-reboot", "-m", "512M",
    "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server,nowait"])
def armed():
    try: return "AETHER_BOOT_OK" in open(serial).read()
    except OSError: return False
for _ in range(250):
    if armed(): break
    if proc.poll() is not None: print("died"); sys.exit(1)
    time.sleep(0.1)
time.sleep(1.5)
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
json.loads(f.readline()); cmd({"execute":"qmp_capabilities"})
goto(576,753); click(); time.sleep(1.2)        # launch Terminal from the Dock (icon 3 of 9)
for line in ["uname\n","ls /bin | wc\n","echo aether-os-is-real > /hi.txt\n","cat /hi.txt\n"]:
    send(line); time.sleep(1.6)
time.sleep(1.5)
cmd({"execute":"screendump","arguments":{"filename":out}}); time.sleep(0.5)
cmd({"execute":"quit"})
try: proc.wait(timeout=5)
except Exception: proc.kill()
ok = b"aether-os-is-real" in open(disk,"rb").read()
for p in (sock,serial):
    try: os.unlink(p)
    except OSError: pass
print("PASS" if ok else "FAIL")
