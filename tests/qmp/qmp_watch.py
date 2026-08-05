#!/usr/bin/env python3
"""Freeze hunter: run the user's sequence; if the screen goes static, dump ALL
CPUs' registers via the QEMU monitor (the frozen RIPs name the culprit), then quit.
Usage: qmp_watch.py <iso> <disk> <outdir> [rounds]"""
import socket, json, sys, os, time, subprocess, tempfile, shutil

iso, disk, outdir = sys.argv[1], sys.argv[2], sys.argv[3]
rounds = int(sys.argv[4]) if len(sys.argv) > 4 else 20
os.makedirs(outdir, exist_ok=True)

def run_once(rnd):
    work = tempfile.mktemp(suffix=".img")
    shutil.copyfile(disk, work)
    fd, sock = tempfile.mkstemp(suffix=".qmp"); os.close(fd); os.unlink(sock)
    serial = os.path.join(outdir, f"watch{rnd}-serial.log")
    args = ["qemu-system-x86_64", "-cdrom", iso,
        "-drive", f"file={work},format=raw,if=ide", "-boot", "d",
        "-m", "512", "-smp", "4", "-accel", "tcg,thread=multi", "-rtc", "base=localtime",
        "-vga", "none", "-device", "virtio-gpu-pci",
        "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
        "-serial", f"file:{serial}", "-no-reboot", "-display", "none",
        "-qmp", f"unix:{sock},server,nowait"]
    proc = subprocess.Popen(args)
    def armed():
        try: return "AETHER_BOOT_OK" in open(serial).read()
        except OSError: return False
    for _ in range(300):
        if armed(): break
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
    def mon(c): return cmd({"execute": "human-monitor-command", "arguments": {"command-line": c}})
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
    def shot():
        p = os.path.join(outdir, "watch.ppm")
        cmd({"execute": "screendump", "arguments": {"filename": p}}); time.sleep(0.3)
        try: return open(p, "rb").read()
        except OSError: return b""
    def frozen_probe():
        a = shot()
        for _ in range(4):
            time.sleep(2.5)
            b = shot()
            if b != a: return False
            a = b
        return True
    def dump_state():
        print(f"=== ROUND {rnd}: FROZEN — dumping CPU registers ===")
        for cpu in range(4):
            mon(f"cpu {cpu}")
            r = mon("info registers")
            print(f"--- CPU{cpu} ---")
            print((r or {}).get("return", ""))
        mon("cpu 0")
        print("--- serial ---")
        try: print(open(serial).read())
        except OSError: pass
        cmd({"execute": "quit"})
        try: proc.wait(timeout=5)
        except Exception: proc.kill()
        os.unlink(work)
        return True

    # user sequence: Studio -> edit -> Terminal -> type
    goto(832, 753); click(); time.sleep(3)
    for q in "print": key(q)
    time.sleep(1)
    if frozen_probe(): return dump_state()
    goto(576, 753); click(); time.sleep(4)
    for q in list("uname") + ["ret"]: key(q)
    time.sleep(2)
    if frozen_probe(): return dump_state()
    cmd({"execute": "quit"})
    try: proc.wait(timeout=5)
    except Exception: proc.kill()
    os.unlink(work)
    print(f"round {rnd} ok", flush=True)
    return False

for r in range(1, rounds + 1):
    if run_once(r): sys.exit(0)
print("ALL_ROUNDS_OK")
