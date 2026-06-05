#!/usr/bin/env python3
"""Screendump a running QEMU via its QMP unix socket.
Usage: qmp_shot.py <qmp.sock> <out.ppm>"""
import socket, json, sys, time

sock, out = sys.argv[1], sys.argv[2]
s = socket.socket(socket.AF_UNIX)
for _ in range(100):
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

json.loads(f.readline())                 # QMP greeting
cmd({"execute": "qmp_capabilities"})
r = cmd({"execute": "screendump", "arguments": {"filename": out}})
print("screendump:", r)
