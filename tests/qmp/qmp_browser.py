#!/usr/bin/env python3
"""Drive the Browser app over QMP: launch from the Dock, load the default URL
(then optionally type one passed in), screenshot.
Usage: qmp_browser.py <qmp.sock> <out.ppm> [url] [wait] [x,y] [scrolls]
When scrolls > 0, also captures out-scroll<N>.ppm after N PageDowns each."""
import socket, json, sys, time

# --- Dock geometry, mirrored from draw_dock() in c/kernel/gui/wm.c ---
# This used to be a hardcoded pair, and it silently rotted: the dock is CENTRED,
# so adding one app moves every icon 32px left and the old coordinate lands on
# the wrong app (or on the wallpaper, which does nothing at all and looks
# exactly like the browser failing to open). Deriving it from the app count
# means the next app costs a one-line edit instead of a silent false pass.
SCREEN_W, SCREEN_H = 1280, 800
DOCK_ISZ, DOCK_GAP = 50, 14           # wm.c: dock_isz, dock_gap
NAPPS = 9                             # *.aex at the LogitFS root, in scan_apps order:
BROWSER_SLOT = 8                      # clock textedit monitor terminal widgets
                                      # files preview studio browser

def dock_icon(i, n=NAPPS):
    """Centre of dock icon `i` (0-based), matching wm.c's draw_dock()."""
    dw = DOCK_GAP + n * (DOCK_ISZ + DOCK_GAP)
    x0 = (SCREEN_W - dw) // 2
    y0 = SCREEN_H - (DOCK_ISZ + 20) - 12
    return (x0 + DOCK_GAP + i * (DOCK_ISZ + DOCK_GAP) + DOCK_ISZ // 2,
            y0 + 10 + DOCK_ISZ // 2)

sock_path, out = sys.argv[1], sys.argv[2]
url = sys.argv[3] if len(sys.argv) > 3 else None
wait = float(sys.argv[4]) if len(sys.argv) > 4 else 11   # post-Enter wait before screendump

s = socket.socket(socket.AF_UNIX)
for _ in range(120):
    try: s.connect(sock_path); break
    except OSError: time.sleep(0.1)
f = s.makefile("rw")

def recv():
    while True:
        line = f.readline()
        if not line: return None
        m = json.loads(line)
        if "return" in m or "error" in m: return m
def cmd(d): f.write(json.dumps(d) + "\n"); f.flush(); return recv()

cur = [640, 400]   # WM centers the cursor at screen center (1280x800)
def goto(tx, ty):
    cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"rel","data":{"axis":"x","value":tx-cur[0]}},
        {"type":"rel","data":{"axis":"y","value":ty-cur[1]}}]}})
    cur[0], cur[1] = tx, ty; time.sleep(0.2)
def click():
    for d in (True, False):
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":d}}]}})
        time.sleep(0.12)
def key(q):
    for d in (True, False):
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"key","data":{"key":{"type":"qcode","data":q},"down":d}}]}})
    time.sleep(0.05)
KMAP = {" ":"spc",".":"dot","\n":"ret","-":"minus","/":"slash"}
SHIFT = {":":"semicolon"}
def key_shift(q):
    cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"key","data":{"key":{"type":"qcode","data":"shift"},"down":True}},
        {"type":"key","data":{"key":{"type":"qcode","data":q},"down":True}}]}})
    cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"key","data":{"key":{"type":"qcode","data":q},"down":False}},
        {"type":"key","data":{"key":{"type":"qcode","data":"shift"},"down":False}}]}})
    time.sleep(0.05)
def typ(t):
    for ch in t:
        if ch in SHIFT: key_shift(SHIFT[ch])
        else: key(KMAP.get(ch, ch))

json.loads(f.readline())
cmd({"execute":"qmp_capabilities"})

# Launch Browser from the Dock (the globe, currently rightmost -> (896, 753)).
# The 5th arg still overrides, for driving a build with a different app set.
dx, dy = (map(int, sys.argv[5].split(","))) if len(sys.argv) > 5 else dock_icon(BROWSER_SLOT)
goto(dx, dy); click()
time.sleep(2.5)          # the .aex is ~2.6 MB off virtio-blk, then ELF load + first paint

# Capture the window BEFORE any URL is typed. Without this, "the dock click
# missed" and "the page failed to render" produce the same blank final image
# and there is nothing to tell them apart after the fact.
base = out.rsplit(".", 1)
launch = base[0] + "-launch." + (base[1] if len(base) > 1 else "ppm")
cmd({"execute":"screendump","arguments":{"filename":launch}})
time.sleep(0.4)

if url:
    # focus the address bar, clear it, type the new URL
    goto(420, 145); click(); time.sleep(0.2)
    for _ in range(60): key("backspace")
    typ(url)

key("ret")                          # load
time.sleep(wait)                    # DNS + TCP (+TLS) + parse + external CSS + layout
cmd({"execute":"screendump","arguments":{"filename":out}})
time.sleep(0.5)

scrolls = int(sys.argv[6]) if len(sys.argv) > 6 else 0
for i in range(1, scrolls + 1):
    for _ in range(3): key("pgdn")  # ~3 viewport pages per capture
    time.sleep(1.0)
    base = out.rsplit(".", 1)
    name = base[0] + "-scroll" + str(i) + "." + (base[1] if len(base) > 1 else "ppm")
    cmd({"execute":"screendump","arguments":{"filename":name}})
    time.sleep(0.5)
print("done")
