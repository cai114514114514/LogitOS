#!/usr/bin/env python3
"""Exercise the Aether app platform over QMP.
Usage: qmp_apps.py <qmp.sock> <a.ppm> <b.ppm>"""
import socket, json, sys, time

sock_path, out_a, out_b = sys.argv[1], sys.argv[2], sys.argv[3]
s = socket.socket(socket.AF_UNIX)
for _ in range(80):
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

cur = [512, 384]
def goto(tx, ty):
    cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"rel","data":{"axis":"x","value":tx-cur[0]}},
        {"type":"rel","data":{"axis":"y","value":ty-cur[1]}}]}})
    cur[0], cur[1] = tx, ty
    time.sleep(0.15)
def click():
    for d in (True, False):
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":d}}]}})
        time.sleep(0.1)
def key(q):
    for d in (True, False):
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"key","data":{"key":{"type":"qcode","data":q},"down":d}}]}})
    time.sleep(0.04)
KMAP = {" ":"spc",".":"dot","\n":"ret","-":"minus"}
def typ(t):
    for ch in t: key(KMAP.get(ch, ch))

json.loads(f.readline())
cmd({"execute":"qmp_capabilities"})

# 1) open readme.txt from Finder -> TextEdit (file association)
#    click the left part of the row, clear of the auto-launched Clock window
goto(60, 138)
click()
time.sleep(0.8)
cmd({"execute":"screendump","arguments":{"filename":out_a}})
time.sleep(0.4)

# 2) launch Terminal from the Dock (4th icon ~ x=608), then run a command
goto(608, 720)
click()
time.sleep(0.8)
typ("ls\n")
typ("uname\n")
time.sleep(0.5)
cmd({"execute":"screendump","arguments":{"filename":out_b}})
time.sleep(0.4)
cmd({"execute":"quit"})
print("qmp_apps: done")
