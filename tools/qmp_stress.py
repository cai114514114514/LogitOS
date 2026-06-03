#!/usr/bin/env python3
"""Stress the Browser over QMP: launch from the Dock, load a URL, then reload it
N times (heavy malloc/free churn). Screenshot at the end.
Usage: qmp_stress.py <qmp.sock> <out.ppm> <url> [reloads]"""
import socket, json, sys, time

sock_path, out, url = sys.argv[1], sys.argv[2], sys.argv[3]
reloads = int(sys.argv[4]) if len(sys.argv) > 4 else 8

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

cur = [512, 384]
def goto(tx, ty):
    cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"rel","data":{"axis":"x","value":tx-cur[0]}},
        {"type":"rel","data":{"axis":"y","value":ty-cur[1]}}]}})
    cur[0], cur[1] = tx, ty; time.sleep(0.2)
def click():
    for d in (True, False):
        cmd({"execute":"input-send-event","arguments":{"events":[
            {"type":"btn","data":{"button":"left","down":d}}]}}); time.sleep(0.12)
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

goto(639, 721); click(); time.sleep(1.2)          # launch Browser from the Dock
goto(300, 112); click(); time.sleep(0.2)          # focus address bar
for _ in range(60): key("backspace")
typ(url)
for i in range(reloads + 1):
    key("ret")                                    # load / reload
    time.sleep(6)
    print(f"reload {i} done", flush=True)
cmd({"execute":"screendump","arguments":{"filename":out}})
time.sleep(0.5)
print("done")
