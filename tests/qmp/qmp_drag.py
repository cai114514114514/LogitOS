#!/usr/bin/env python3
"""Drive the Logit OS PS/2 mouse over QMP to demonstrate window dragging,
then screendump. Usage: qmp_drag.py <qmp.sock> <out.ppm>"""
import socket
import json
import sys
import time

sock_path, outfile = sys.argv[1], sys.argv[2]

s = socket.socket(socket.AF_UNIX)
for _ in range(50):
    try:
        s.connect(sock_path)
        break
    except OSError:
        time.sleep(0.1)
f = s.makefile("rw")


def recv_return():
    while True:
        line = f.readline()
        if not line:
            return None
        msg = json.loads(line)
        if "return" in msg or "error" in msg:
            return msg
        # ignore async "event" messages


def cmd(d):
    f.write(json.dumps(d) + "\n")
    f.flush()
    return recv_return()


def move(dx, dy):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "rel", "data": {"axis": "x", "value": dx}},
        {"type": "rel", "data": {"axis": "y", "value": dy}}]}})
    time.sleep(0.05)


def button(down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": down}}]}})
    time.sleep(0.08)


json.loads(f.readline())             # QMP greeting
cmd({"execute": "qmp_capabilities"})

move(-262, -232)                     # center (512,384) -> Finder title (~250,152)
time.sleep(0.25)
button(True)                         # grab Finder's title bar (raises it)
for _ in range(8):
    move(22, 30)                     # drag down-right
time.sleep(0.25)
button(False)                        # drop
time.sleep(0.5)

cmd({"execute": "screendump", "arguments": {"filename": outfile}})
time.sleep(0.4)
cmd({"execute": "quit"})
print("qmp_drag: done")
