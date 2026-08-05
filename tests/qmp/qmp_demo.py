#!/usr/bin/env python3
"""Drive Logit OS over QMP: type shell commands, then click a file in Finder.
Usage: qmp_demo.py <qmp.sock> <out_a.ppm> <out_b.ppm>"""
import socket, json, sys, time

sock_path, out_a, out_b = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try: s.connect(sock_path); break
    except OSError: time.sleep(0.1)
f = s.makefile("rw")

def recv():
    while True:
        line = f.readline()
        if not line: return None
        m = json.loads(line)
        if "return" in m or "error" in m: return m

def cmd(d):
    f.write(json.dumps(d) + "\n"); f.flush(); return recv()

def key(q):
    for down in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": down}}]}})
    time.sleep(0.03)

KMAP = {" ": "spc", ".": "dot", "\n": "ret", "-": "minus"}
def typ(text):
    for ch in text:
        key(KMAP.get(ch, ch))

def move(dx, dy):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "rel", "data": {"axis": "x", "value": dx}},
        {"type": "rel", "data": {"axis": "y", "value": dy}}]}})
    time.sleep(0.05)

def click():
    for down in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]}})
        time.sleep(0.08)

json.loads(f.readline())
cmd({"execute": "qmp_capabilities"})

# --- terminal: run a couple of shell commands (Terminal is focused at boot) ---
typ("ls\n")
typ("cat readme.txt\n")
typ("mem\n")
time.sleep(0.6)
cmd({"execute": "screendump", "arguments": {"filename": out_a}})
time.sleep(0.4)

# --- click readme.txt in Finder (row 0 ~ (100,128)) to open it in the Viewer ---
move(-412, -256)          # center (512,384) -> ~ (100,128)
time.sleep(0.2)
click()
time.sleep(0.6)
cmd({"execute": "screendump", "arguments": {"filename": out_b}})
time.sleep(0.4)
cmd({"execute": "quit"})
print("qmp_demo: done")
