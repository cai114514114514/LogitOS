#!/usr/bin/env python3
"""Freeze repro: CHURN kernel heartbeat + hostile real-input fuzz over QMP.
The WM loop prints [s] every 32 churn steps; if heartbeats stop -> FROZEN.
Usage: qmp_fuzz.py <qmp.sock> <serial.log> <seconds>"""
import socket, json, sys, time, random, threading, os

sock_path, serial_log, seconds = sys.argv[1], sys.argv[2], int(sys.argv[3])
random.seed(1234)

s = socket.socket(socket.AF_UNIX)
for _ in range(150):
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

json.loads(f.readline())
cmd({"execute": "qmp_capabilities"})

cur = [640, 400]
def goto(tx, ty):
    while cur[0] != tx or cur[1] != ty:
        sx = max(-100, min(100, tx - cur[0])); sy = max(-100, min(100, ty - cur[1]))
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "rel", "data": {"axis": "x", "value": sx}},
            {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
        cur[0] += sx; cur[1] += sy; time.sleep(0.02)
def btn(down):
    cmd({"execute": "input-send-event", "arguments": {"events": [
        {"type": "btn", "data": {"button": "left", "down": down}}]}})
    time.sleep(0.05)
def click():
    btn(True); btn(False)
def key(q):
    for d in (True, False):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": d}}]}})
        time.sleep(0.02)

frozen = threading.Event()
def watch():
    last_n, last_t = -1, time.time()
    while not frozen.is_set():
        try: n = open(serial_log, "rb").read().count(b"[s]")
        except OSError: n = -1
        now = time.time()
        if n != last_n: last_n, last_t = n, now
        elif now - last_t > 20 and last_n >= 0:   # no heartbeat for 20s -> dead
            frozen.set(); print("FROZEN detected: heartbeat stuck at", last_n, flush=True); return
        time.sleep(2)
threading.Thread(target=watch, daemon=True).start()

DOCK = [(384 + i * 64, 753) for i in range(9)]
KEYS = list("abcdef012345") + ["ret", "spc", "backspace", "tab"]

t0 = time.time(); i = 0
while time.time() - t0 < seconds and not frozen.is_set():
    i += 1
    r = random.random()
    if r < 0.25:      # dock click (launch/focus apps incl. terminal idx 3)
        x, y = random.choice(DOCK); goto(x, y); click()
    elif r < 0.45:    # click somewhere random (title bars, close buttons, content)
        goto(random.randint(0, 1279), random.randint(24, 790)); click()
    elif r < 0.65:    # drag: down, move, up
        goto(random.randint(100, 1100), random.randint(60, 500)); btn(True)
        goto(random.randint(100, 1100), random.randint(60, 500)); btn(False)
    elif r < 0.85:    # mash keys into whatever is focused
        for _ in range(random.randint(1, 12)): key(random.choice(KEYS))
    else:             # wild cursor wiggle (IRQ storm)
        for _ in range(20): goto(random.randint(0, 1279), random.randint(24, 799))
    if i % 20 == 0: print(f"iter {i} t={int(time.time()-t0)}s", flush=True)

print("FUZZ_DONE frozen=" + str(frozen.is_set()), flush=True)
