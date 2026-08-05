#!/usr/bin/env python3
"""Minimal QMP probe: move cursor to (985,752), click, wait, screenshot.
Verifies PS/2 input delivery + dock click behavior headlessly."""
import socket, json, sys, time

sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
for _ in range(120):
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

json.loads(f.readline())
cmd({"execute":"qmp_capabilities"})

cur = [640, 400]
def goto(tx, ty):
    r = cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"rel","data":{"axis":"x","value":tx-cur[0]}},
        {"type":"rel","data":{"axis":"y","value":ty-cur[1]}}]}})
    cur[0], cur[1] = tx, ty; time.sleep(0.3)
    return r

print("goto:", goto(985, 752))
for d in (True, False):
    print("btn:", cmd({"execute":"input-send-event","arguments":{"events":[
        {"type":"btn","data":{"button":"left","down":d}}]}}))
    time.sleep(0.15)
time.sleep(2.5)
print("dump:", cmd({"execute":"screendump","arguments":{"filename":out}}))
print("done")
