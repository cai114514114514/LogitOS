#!/usr/bin/env python3
"""LogitFS v3 UI smoke test, self-contained and reproducible.

Boots the ISO headless with a QMP socket, waits until the kernel is armed
(serial prints LOGIT_BOOT_OK -- injecting input before mouse_init() silently
drops events), drives the Terminal over QMP to create a nested file, screenshots,
then inspects the raw disk image to confirm the write persisted.

Usage: qmp_fs.py <logit.iso> <disk.img> [out.ppm]

Notes baked in from debugging this stack:
  - wait for LOGIT_BOOT_OK before sending any input;
  - QEMU qcodes are 'ctrl'/'shift' (NOT 'ctrl_l'/'shift_l');
  - PS/2 relative motion is clamped to ~9 bits, so step moves <=200 px.
  - the Dock's layout is NOT stated here any more. This comment used to spell
    the mkfs packing order out -- "clock(0) textedit(1) monitor(2) terminal(3)
    widgets(4) files(5) preview(6) studio(7) browser(8), icon i centre x =
    384 + i*64" -- for a NINE-icon dock, and the disk now ships eleven. That
    is not a hypothetical: under the eleven-icon dock this file's
    goto(576, 753) landed EXACTLY on widgets.aex's tile (centre 512+64), so
    the "Terminal" commands below were being typed into the Widgets window
    and the disk probe could never appear. It now reads the tile off the
    guest's own [wm] dock line and refuses to continue unless the guest says
    the click launched Terminal. See tests/qmp/qmp_ui.py's dock block.
"""
import socket, json, shutil, sys, os, time, subprocess, tempfile, atexit

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
import qmp_ui                                    # noqa: E402
from qmp_ui import LAUNCH_RE, dock_icon_of, parse_dock, title_of  # noqa: E402

iso, disk = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else "build/fs_smoke.ppm"
# The persistence assertion at the bottom reads THIS file, not the caller's
# image: this driver used to boot with -snapshot, under which every guest
# write lands in a temporary overlay and the backing image can NEVER contain
# the probe -- an assertion that could only ever fail, on a driver nothing
# wired up, so it failed unobserved. A private copy boots writable and is
# thrown away whole.
work = tempfile.mkdtemp(prefix="logit-fs-")
# "thrown away whole" is what the comment above PROMISES; until this line it
# was never thrown away at all, and the copy below is 512 MB. atexit rather
# than a finally: the dir is created at module scope, so there is no block to
# attach to, and this also covers the fail() paths that sys.exit() out early.
# Registered BEFORE the copyfile, not after: a copy that raises (a missing or
# unreadable disk image) would otherwise leave the directory behind having
# never reached the registration -- measured, not reasoned about.
atexit.register(shutil.rmtree, work, ignore_errors=True)
private_disk = os.path.join(work, "disk.img")
shutil.copyfile(disk, private_disk)
fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)  # QEMU binds the socket itself
fd, serial = tempfile.mkstemp(suffix=".log"); os.close(fd)
qemu = os.environ.get("QEMU", "qemu-system-x86_64")

proc = subprocess.Popen([
    qemu, "-cpu", "max", "-cdrom", iso,
    "-drive", f"file={private_disk},format=raw,if=ide,index=0,media=disk", "-boot", "d",
    # WRITABLE -- no -snapshot: see the private_disk comment above.
    # The dock arithmetic assumes 1280x800 (qmp_ui's default). Pinned here
    # explicitly because it used to be an accident of the default framebuffer.
    "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
    "-display", "none", "-no-reboot",
    "-serial", f"file:{serial}", "-qmp", f"unix:{sock},server,nowait",
])

def armed():
    try:
        with open(serial, encoding="utf-8", errors="replace") as fh:
            return "LOGIT_BOOT_OK" in fh.read()
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
    fail("LOGIT_BOOT_OK never appeared")
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

cur = [640, 400]                              # kernel cursor starts at screen center
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

# Launch Terminal from the Dock -- tile read off the guest's own [wm] dock
# line (see the header), then VERIFIED: the [wm] launched line must name
# Terminal. Without the check this driver's failure mode was silent: a stale
# coordinate typed mkdir into whatever neighbouring app the click landed on,
# and the only symptom was the disk probe never appearing at the very end.
dock = None
for _ in range(300):                       # the line is printed by wm_init()
    dock = parse_dock(open(serial, errors="replace").read())
    if dock:
        break
    if proc.poll() is not None:
        fail("qemu exited before the WM published its dock")
    time.sleep(0.2)
if dock is None:
    fail("no [wm] dock line on serial -- is this disk the one wm.c dock_publish() ships on?")
tx, ty = dock_icon_of("terminal", dock)
mark = len(open(serial, errors="replace").read())
goto(tx, ty); click(); time.sleep(1.0)
got = [m.group(1).strip() for m in LAUNCH_RE.finditer(open(serial, errors="replace").read()[mark:])]
if got != [title_of("terminal")]:
    fail("clicked Terminal's dock tile at (%d,%d): guest launched %r (expected %r)"
         % (tx, ty, got, title_of("terminal")))
# The launched line means the process EXISTS; the shell it runs has to exec
# before the first keystroke means anything. 3 s measured enough under TCG
# (the login -> sh exec chain), and a keystroke dropped here looks exactly
# like a filesystem that lost the file.
time.sleep(3.0)

for line in ["mkdir proj\n", "cd proj\n", "echo smokeprobe > note.txt\n", "ls\n", "cat note.txt\n"]:
    send(line); time.sleep(0.5)
time.sleep(0.4)
cmd({"execute": "screendump", "arguments": {"filename": out}}); time.sleep(0.4)
cmd({"execute": "quit"})
try: proc.wait(timeout=5)
except Exception: proc.kill()

with open(private_disk, "rb") as fh:
    blob = fh.read()
ok = b"smokeprobe" in blob and b"proj" in blob and b"note.txt" in blob
if ok:
    os.unlink(sock) if os.path.exists(sock) else None
    os.unlink(serial) if os.path.exists(serial) else None
    print("PASS: /proj/note.txt created and persisted")
else:
    # Evidence survives a failure: the serial log is the only record of what
    # the guest did with the keystrokes, and this driver used to unlink it
    # before printing its verdict.
    print("FAIL: probe not found on the booted copy (serial log kept at %s)"
          % serial)
sys.exit(0 if ok else 1)
