#!/usr/bin/env python3
"""Real-guest acceptance for input while an initial page load is in flight.

The fixture server deliberately holds a stylesheet, initial script, or image.
That delay is only a stimulus.  Closure is proved by the guest WM's exact
window id becoming ``gone``; scrolling is proved from an odd-colour box moving
in QEMU scanout.  No page script, host stopwatch, or guessed window coordinate
is the oracle.  Every wheel case proves the scanout moves *before* the held
response is released; waiting until load completes would only prove that the
event queued.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time
import zlib
import struct

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tests/qmp"))
from qmp_ui import PPM, Session  # noqa: E402

ANCHOR = (18, 171, 52)


def png():
    def chunk(kind, body):
        return (struct.pack("!I", len(body)) + kind + body +
                struct.pack("!I", zlib.crc32(kind + body)))
    scan = (b"\0" + bytes((34, 119, 187)) * 32) * 32
    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack("!2I5B", 32, 32, 8, 2, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(scan)) + chunk(b"IEND", b""))


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    releases = {"css": threading.Event(), "scroll_css": threading.Event(),
                "scroll_script": threading.Event(), "image": threading.Event()}
    reached = {"css": threading.Event(), "scroll_css": threading.Event(),
               "scroll_script": threading.Event(), "image": threading.Event()}
    requests = []

    def send(self, data, content_type):
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(data)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        self.requests.append(path)
        if path == "/held.css":
            self.reached["css"].set()
            self.releases["css"].wait(30)
            self.send(b"body{background:#f7f8fa}", "text/css")
            return
        if path == "/held-scroll.css":
            self.reached["scroll_css"].set()
            self.releases["scroll_css"].wait(30)
            self.send(b"body{background:#f4f5f7}", "text/css")
            return
        if path == "/held-scroll.js":
            self.reached["scroll_script"].set()
            self.releases["scroll_script"].wait(30)
            self.send(b"console.log('SCROLL-HELD-SCRIPT-RAN')",
                      "application/javascript")
            return
        if path == "/held.png":
            self.reached["image"].set()
            self.releases["image"].wait(30)
            self.send(png(), "image/png")
            return
        if path == "/close-css":
            body = b"""<!doctype html><title>Close while CSS loads</title>
<link rel=stylesheet href=/held.css><h1>CLOSE-CSS-PAGE</h1>
<script>console.log('CLOSE-CSS-SCRIPT-SHOULD-NOT-RUN-EARLY')</script>"""
            self.send(body, "text/html; charset=utf-8")
            return
        if path == "/busy-js":
            body = b"""<!doctype html><title>Close during JavaScript</title>
<h1>BUSY-JS-PAGE</h1><script>
console.log('BUSY-JS-ENTER');for(;;){}
</script>"""
            self.send(body, "text/html; charset=utf-8")
            return
        if path == "/scroll-css":
            body = b"""<!doctype html><title>Wheel while CSS loads</title>
<link rel=stylesheet href=/held-scroll.css>
<body style='margin:0;background:#f4f5f7'>
<div style='height:280px'>TOP OF TALL PAGE</div>
<div style='width:220px;height:90px;background:#12ab34;color:#000'>SCROLL CSS ANCHOR</div>
<div style='height:1700px'>TALL PAGE FILLER</div>
<script>console.log('SCROLL-CSS-LOADED')</script>"""
            self.send(body, "text/html; charset=utf-8")
            return
        if path == "/scroll-script":
            body = b"""<!doctype html><title>Wheel while script loads</title>
<body style='margin:0;background:#f4f5f7'>
<div style='height:280px'>TOP OF TALL PAGE</div>
<div style='width:220px;height:90px;background:#12ab34;color:#000'>SCROLL SCRIPT ANCHOR</div>
<div style='height:1700px'>TALL PAGE FILLER</div>
<script src=/held-scroll.js></script>"""
            self.send(body, "text/html; charset=utf-8")
            return
        if path == "/scroll-image":
            body = b"""<!doctype html><title>Wheel during load</title>
<body style='margin:0;background:#f4f5f7'>
<div style='height:280px'>TOP OF TALL PAGE</div>
<div style='width:220px;height:90px;background:#12ab34;color:#000'>SCROLL LOAD ANCHOR</div>
<div style='height:1700px'>TALL PAGE FILLER</div>
<img width=32 height=32 src=/held.png>
<script>console.log('SCROLL-SCRIPT');window.addEventListener('load',function(){console.log('SCROLL-LOADED')})</script>"""
            self.send(body, "text/html; charset=utf-8")
            return
        self.send(b"not found", "text/plain")

    def log_message(self, *_args):
        pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / "serial.txt"
    serial.write_text("")

    inputs = [pathlib.Path(args.iso).resolve(), pathlib.Path(args.disk).resolve()]

    def ids():
        return {str(p): {"sha256": hashlib.file_digest(p.open("rb"), "sha256").hexdigest(),
                         "size": p.stat().st_size, "mtime_ns": p.stat().st_mtime_ns}
                for p in inputs}

    before = ids()
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    result = {"artifacts": {"before": before}, "checks": []}

    with tempfile.TemporaryDirectory(prefix="bli-", dir="/tmp") as tmp:
        sock = str(pathlib.Path(tmp) / "qmp")
        cmd = ["qemu-system-x86_64", "-cpu", "max", "-cdrom", args.iso,
               "-drive", f"file={args.disk},format=raw,if=none,id=hd0,file.locking=off",
               "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
               "-m", "1G", "-smp", "4", "-accel", "tcg,thread=multi",
               "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
               "-display", "none", "-no-reboot", "-netdev", "user,id=n0",
               "-device", "e1000,netdev=n0", "-serial", "file:" + str(serial),
               "-qmp", "unix:" + sock + ",server,nowait"]
        with (out / "qemu.log").open("w") as qlog:
            proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)

            def text():
                return serial.read_text(errors="replace")

            def wait(marker, start=0, seconds=120):
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    tail = text()[start:]
                    if marker in tail and "\n" in tail[tail.index(marker) + len(marker):]:
                        return tail
                    if proc.poll() is not None:
                        raise RuntimeError("QEMU exited before " + marker)
                    time.sleep(.12)
                raise RuntimeError("guest marker missing: " + marker)

            def frame():
                rows = re.findall(r"\[wm\] win (\d+) frame (-?\d+) (-?\d+) (\d+) (\d+) "
                                  r"content \d+ \d+ pt[^\n]* Browser\r?", text(), re.I)
                if not rows:
                    raise RuntimeError("Browser frame not reported")
                return tuple(map(int, rows[-1]))

            def nav(ui, path):
                ui.key_mods(("ctrl",), "l", settle=.2)
                ui.typ(f"http://10.0.2.2:{srv.server_port}{path}")
                ui.key("ret")

            def close_and_relaunch(ui, label):
                wid, x, y, _w, _h = frame()
                mark = len(text())
                start = time.monotonic()
                ui.click_at(x + 16, y + 15, settle=.05)
                wait(f"[wm] win {wid} gone", mark, seconds=8)
                elapsed = (time.monotonic() - start) * 1000
                result["checks"].append({"case": label, "window_id": wid,
                                         "wm_gone_host_bound_ms": round(elapsed, 1)})
                relaunch_mark = len(text())
                ui.launch_app("browser", timeout=90)
                wait("[wm] launched Browser", relaunch_mark, seconds=90)
                return elapsed

            def wheel_burst(ui):
                for _ in range(4):
                    for down in (True, False):
                        ui._input([{"type": "btn", "data": {
                            "button": "wheel-down", "down": down}}])

            def wait_live_scroll(label, before_box, held, seconds=5.0):
                started = time.monotonic()
                deadline = started + seconds
                during = None
                probe = 0
                while time.monotonic() < deadline:
                    if held.is_set():
                        raise AssertionError(label + " response released before visual proof")
                    time.sleep(.15)
                    shot = out / f"{label}-during-{probe}.ppm"
                    ui.screendump(str(shot), settle=0)
                    during = PPM(str(shot)).find_color(ANCHOR)
                    if during is not None and during[1] <= before_box[1] - 80:
                        return during, (time.monotonic() - started) * 1000
                    probe += 1
                raise AssertionError(
                    f"wheel did not move scanout while {label} stayed held: "
                    f"{before_box} -> {during}")

            try:
                wait("desktop live", seconds=180)
                time.sleep(3)
                ui = Session(sock, serial=str(serial))
                ui.launch_app("browser", timeout=120)
                time.sleep(2)

                nav(ui, "/close-css")
                if not Fixture.reached["css"].wait(30):
                    raise RuntimeError("held stylesheet was never requested")
                close_and_relaunch(ui, "close_during_held_stylesheet")
                Fixture.releases["css"].set()
                time.sleep(1)

                mark = len(text())
                nav(ui, "/busy-js")
                wait("BUSY-JS-ENTER", mark, seconds=60)
                close_and_relaunch(ui, "close_during_busy_javascript")

                # Put the pointer in the page before navigation. Moving it after
                # the synchronous CSS wait starts would correctly put an older
                # EV_MOUSE_MOVE ahead of the wheel in the loader FIFO.
                ui.goto(500, 400)
                time.sleep(.5)
                mark = len(text())
                nav(ui, "/scroll-css")
                if not Fixture.reached["scroll_css"].wait(45):
                    raise RuntimeError("held scrolling stylesheet was never requested")
                time.sleep(.4)
                css_before_shot = out / "scroll-css-before.ppm"
                ui.screendump(str(css_before_shot), settle=.1)
                css_b0 = PPM(str(css_before_shot)).find_color(ANCHOR)
                if css_b0 is None:
                    raise RuntimeError("CSS scroll anchor absent before stylesheet completed")
                wheel_burst(ui)
                css_bd, css_live_ms = wait_live_scroll(
                    "scroll-css", css_b0, Fixture.releases["scroll_css"])
                # The screen moved while the HTTP handler still held the body;
                # release only after recording that visual oracle.
                Fixture.releases["scroll_css"].set()
                wait("SCROLL-CSS-LOADED", mark, seconds=90)
                result["checks"].append({
                    "case": "wheel_during_held_stylesheet",
                    "response_released_after_visual_proof": True,
                    "before_box": css_b0, "during_box": css_bd,
                    "moved_during_load_px": css_b0[1] - css_bd[1],
                    "live_scroll_host_bound_ms": round(css_live_ms, 1)})

                ui.goto(500, 400)
                time.sleep(.5)
                mark = len(text())
                nav(ui, "/scroll-script")
                if not Fixture.reached["scroll_script"].wait(45):
                    raise RuntimeError("held scrolling script was never requested")
                time.sleep(.4)
                script_before_shot = out / "scroll-script-before.ppm"
                ui.screendump(str(script_before_shot), settle=.1)
                script_b0 = PPM(str(script_before_shot)).find_color(ANCHOR)
                if script_b0 is None:
                    raise RuntimeError("script scroll anchor absent before script completed")
                wheel_burst(ui)
                script_bd, script_live_ms = wait_live_scroll(
                    "scroll-script", script_b0, Fixture.releases["scroll_script"])
                Fixture.releases["scroll_script"].set()
                wait("SCROLL-HELD-SCRIPT-RAN", mark, seconds=90)
                result["checks"].append({
                    "case": "wheel_during_held_initial_script",
                    "response_released_after_visual_proof": True,
                    "before_box": script_b0, "during_box": script_bd,
                    "moved_during_load_px": script_b0[1] - script_bd[1],
                    "live_scroll_host_bound_ms": round(script_live_ms, 1)})

                mark = len(text())
                nav(ui, "/scroll-image")
                if not Fixture.reached["image"].wait(45):
                    raise RuntimeError("held image was never requested")
                time.sleep(.4)
                before_shot = out / "scroll-before.ppm"
                ui.screendump(str(before_shot), settle=.1)
                b0 = PPM(str(before_shot)).find_color(ANCHOR)
                if b0 is None:
                    raise RuntimeError("scroll anchor absent before held image completed")
                cx = (b0[0] + b0[2]) // 2
                cy = (b0[1] + b0[3]) // 2
                ui.goto(cx, cy)
                wheel_burst(ui)
                # QMP's screendump captures first and sleeps afterwards, so one
                # immediate dump can photograph the frame preceding the wheel.
                # Poll a short, explicit host-time bound while the HTTP handler
                # still holds the image.  The visual oracle must move before we
                # release that response; otherwise this is deferred replay, not
                # live scrolling during load.
                bd, live_ms = wait_live_scroll(
                    "scroll-image", b0, Fixture.releases["image"])
                Fixture.releases["image"].set()
                wait("SCROLL-LOADED", mark, seconds=90)
                time.sleep(1.2)
                after_shot = out / "scroll-after.ppm"
                ui.screendump(str(after_shot), settle=.1)
                b1 = PPM(str(after_shot)).find_color(ANCHOR)
                if b1 is None or b1[1] > b0[1] - 80:
                    raise AssertionError(f"wheel movement did not survive load completion: {b0} -> {b1}")
                result["checks"].append({"case": "wheel_during_held_image",
                                         "response_released_after_visual_proof": True,
                                         "before_box": b0, "during_box": bd,
                                         "after_box": b1,
                                         "moved_during_load_px": b0[1] - bd[1],
                                         "moved_after_load_px": b0[1] - b1[1],
                                         "live_scroll_host_bound_ms": round(live_ms, 1)})
                result["passed"] = True
            finally:
                Fixture.releases["css"].set()
                Fixture.releases["scroll_css"].set()
                Fixture.releases["scroll_script"].set()
                Fixture.releases["image"].set()
                proc.terminate()
                try:
                    proc.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    proc.kill(); proc.wait()
                srv.shutdown()

    after = ids()
    result["artifacts"]["after"] = after
    result["artifacts"]["unchanged"] = before == after
    result["requests"] = Fixture.requests
    if not result["artifacts"]["unchanged"]:
        raise RuntimeError("input artifacts changed during run")
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
