#!/usr/bin/env python3
"""Black-frame hunter v2: repeated app open/close cycles (resource wear), then
Terminal. Healthy = the terminal grid shows text (bright pixels in the window
content area of the screendump). Black-frame = window drawn, grid empty.
On failure: screenshot + serial tail are saved for analysis.
Usage: qmp_blackframe.py <iso> <disk> <outdir> [rounds] [cycles]"""
import socket, json, sys, os, time, subprocess, tempfile, shutil

iso, disk, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 10
cycles = int(sys.argv[5]) if len(sys.argv) > 5 else 4
os.makedirs(outdir, exist_ok=True)

def run_once(rnd):
    work = tempfile.mktemp(suffix=".img")
    shutil.copyfile(disk, work)
    fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
    serial = os.path.join(outdir, f"bf{rnd}-serial.log")
    args = ["qemu-system-x86_64", "-cpu", "max", "-cdrom", iso,
        "-drive", f"file={work},format=raw,if=none,id=hd0",
        "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
        "-m", "512", "-smp", "4", "-accel", "tcg,thread=multi", "-rtc", "base=localtime",
        "-vga", "none", "-device", "virtio-gpu-pci",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-serial", f"file:{serial}", "-no-reboot", "-display", "none",
        "-qmp", f"unix:{sock},server,nowait"]
    proc = subprocess.Popen(args)
    def read_serial():
        try: return open(serial, encoding="utf-8", errors="replace").read()
        except OSError: return ""
    for _ in range(300):
        if "LOGIT_BOOT_OK" in read_serial(): break
        if proc.poll() is not None: print("QEMU_DIED"); return False
        time.sleep(0.1)
    time.sleep(2)
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
    json.loads(f.readline()); cmd({"execute": "qmp_capabilities"})
    cur = [640, 400]
    def goto(tx, ty):
        while cur[0] != tx or cur[1] != ty:
            sx = max(-120, min(120, tx - cur[0])); sy = max(-120, min(120, ty - cur[1]))
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "rel", "data": {"axis": "x", "value": sx}},
                {"type": "rel", "data": {"axis": "y", "value": sy}}]}})
            cur[0] += sx; cur[1] += sy; time.sleep(0.04)
        time.sleep(0.15)
    def btn(down):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"button": "left", "down": down}}]}}); time.sleep(0.1)
    def click(): btn(True); btn(False)
    def key(q):
        for d in (True, False):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "key", "data": {"key": {"type": "qcode", "data": q}, "down": d}}]}})
            time.sleep(0.05)
    def shot(name):
        p = os.path.join(outdir, name)
        cmd({"execute": "screendump", "arguments": {"filename": p}}); time.sleep(0.4)
        try: return open(p, "rb").read()
        except OSError: return b""
    def ppm_pixels(d):
        parts = d.split(b"\n", 3)
        w, h = map(int, parts[1].split())
        return w, h, parts[3]
    def grid_text_pixels(d):
        """count bright pixels inside the terminal window content area"""
        w, h, raw = ppm_pixels(d)
        # cascade slot for the newest window depends on cycle count; just scan the
        # whole screen for large dark windows and check the front-most region:
        # simpler: scan rows for the terminal's black grid and count text pixels.
        # The terminal grid is ~580x392 black; text pixels are bright (sum>380).
        best = 0
        # coarse scan: every 4th pixel
        for y in range(0, h, 4):
            row = y * w * 3
            run_dark = 0
            bright_in_run = 0
            for x in range(0, w, 4):
                i = row + x * 3
                r, g, b = raw[i], raw[i+1], raw[i+2]
                if r < 40 and g < 40 and b < 40:
                    run_dark += 1
                else:
                    if run_dark > 30:  # inside a dark expanse
                        if r + g + b > 380: bright_in_run += 1
                    else:
                        run_dark = 0; bright_in_run = 0
            best += bright_in_run
        return best

    def fail(reason):
        print(f"=== ROUND {rnd}: {reason} ===")
        shot(f"bf{rnd}-fail.ppm")
        print("--- serial tail ---")
        print(read_serial()[-3000:])
        cmd({"execute": "quit"})
        try: proc.wait(timeout=5)
        except Exception: proc.kill()
        os.unlink(work)
        return True

    # wear pass: open+close Studio and Monitor `cycles` times (cascade drifts,
    # so recompute close-button positions per open count from boot:
    # Finder=0 Clock=1, each open takes the next slot mod 6).
    open_count = 2  # Finder + Clock at boot
    for c in range(cycles):
        slot = open_count % 6
        wx, wy = 110 + slot * 28, 70 + slot * 28
        goto(832, 753); click(); time.sleep(2.5)     # Studio
        for q in "xy": key(q)
        goto(wx + 16, wy + 15); click(); time.sleep(1.2)   # close it
        open_count += 1
        slot = open_count % 6
        wx, wy = 110 + slot * 28, 70 + slot * 28
        goto(512, 753); click(); time.sleep(2)       # Monitor
        goto(wx + 16, wy + 15); click(); time.sleep(1.2)   # close it
        open_count += 1

    goto(576, 753); click()                          # Terminal
    time.sleep(8)
    d = shot(f"bf{rnd}-term.ppm")
    n = grid_text_pixels(d)
    print(f"round {rnd}: text pixels = {n}", flush=True)
    if n < 20:
        return fail("BLACK-FRAME: terminal grid has no text")
    for q in list("uname") + ["ret"]: key(q)
    time.sleep(3)
    cmd({"execute": "quit"})
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    os.unlink(work)
    return False

for r in range(1, rounds + 1):
    if run_once(r): sys.exit(0)
print("ALL_ROUNDS_OK")
