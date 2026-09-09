#!/usr/bin/env python3
"""ATTACK DRIVER (topic: anim) -- the animation inventory, measured on the glass.

    python3 tests/qmp/qmp_attack_anim.py <iso> <disk.img> [--out DIR] [--only p1,p2]
    python3 tests/qmp/qmp_attack_anim.py --oracle [--out DIR] [--only p1,p2]

Four fixtures under tests/fixtures/attack/anim/ (trans, kf, raf, waapi), each
a grid of 60x60 boxes in unique colours that move 200 px / fade / recolour
over 1 s under one spelling of "animate this". The driver screendumps at the
page's own serial markers (GO/T250/T600/T1200/T2500), reads every box's
position, size, alpha and colour off the picture, and reads each picture's
OWN guest-clock stamp (stamp.js: a bar 5 ms per px) so the numbers are
plotted against the guest time the frame was really captured at, not the
host time the driver hoped for.

The oracle is headless Chrome driven over CDP in REAL time (--oracle): the
same pages, the same __log lines (Chrome's computed values at its own
T250/T600/T1200), and an end-state screenshot measured by the same code.
Headless Chrome's --virtual-time-budget was tried first and REJECTED: under
it every animation timeline is frozen (computed transform stayed matrix(1,0,
0,1,0,0) and a WAAPI animation's currentTime stayed 0 across a 5 s budget;
measured 2026-09-02), so it can only ever show t=0. The analytic oracle --
linear/ease/steps/delay arithmetic done here in Python, not by the engine
under test -- is the third reading and is printed beside both.

This driver ASSERTS NOTHING. It is an inventory: every number is printed and
written to result.json, and the failure classes are drawn from the numbers
in the report, not from a threshold in here.

Nothing here branches on a site, a framework, or a bundle. The fixtures are
the subject; the classes are generic.
"""

import base64
import http.server
import json
import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIX = os.path.join(ROOT, "tests", "fixtures", "attack", "anim")

ANCHOR = (99, 7, 201)      # page origin: the anchor box sits at page (10,10)
CLKBAR = (7, 99, 201)      # the stamp bar: width px * 5 = performance.now() ms
PAGES = ["trans", "kf", "raf", "waapi"]
PREFIX = {"trans": "TR", "kf": "KF", "raf": "RAF", "waapi": "WA", "one": "ONE"}
TRIGGERED = {"trans": True, "kf": False, "raf": False, "waapi": True, "one": True}


# ---------------------------------------------------------------- fixture server
def spec_path(spec):
    """'one?prop=op&nostamp=1' -> 'one-prop=op-nostamp=1.html'.

    The switches ride in the PATH, not a query string, because the guest URL
    is typed through QMP key events and qmp_ui.typ() has no key for '&': it
    was dropped silently and 'prop=op&nostamp=1' arrived as 'prop=opnostamp=1'
    (measured on the first bisect run). '-' and '=' are typeable."""
    page, _q, query = spec.partition("?")
    return page + "".join("-" + kv for kv in query.split("&") if kv) + ".html"


def path_spec(name):
    """The inverse: 'one-prop=op-nostamp=1.html' -> ('one', '?prop=op&nostamp=1')."""
    base = name[:-5] if name.endswith(".html") else name
    parts = base.split("-")
    return parts[0], ("?" + "&".join(parts[1:])) if len(parts) > 1 else ""


def one_css(query):
    """one.html's CSS, built HERE (the fixture server) from the switches -- the
    first version used document.write('<style>...') and the guest ignored a
    sheet written that way (the box painted alpha 1.0 with `opacity:.2` in the
    logged CSS), which is a finding for another topic and a broken apparatus
    for this one."""
    prop = (re.search(r"prop=(\w+)", query) or [None, "op"])[1]
    zero = "base=zero" in query
    css = "#o { "
    if prop in ("op", "both"): css += "opacity: .2; "
    if zero: css += "transform: translateX(0); "
    tr = {"op": "opacity 1s linear", "tf": "transform 1s linear"}.get(prop, "opacity 1s linear, transform 1s linear")
    css += "transition: " + tr + "; }\n#o.go { "
    css += {"op": "opacity: 1;", "tf": "transform: translateX(200px);"}.get(prop, "opacity: 1; transform: translateX(200px);")
    css += " }\n"
    kind = {"op": "op_lin", "tf": "xf_lin"}.get(prop, "xf_lin+op_lin")
    return css, kind


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        name = os.path.basename(self.path.split("?")[0])
        base, query = path_spec(name) if name.endswith(".html") else (name, "")
        path = os.path.join(FIX, (base + ".html") if name.endswith(".html") else name)
        if not (name.endswith(".html") or name.endswith(".js")) or not os.path.exists(path):
            self.send_response(404)
            self.end_headers()
            return
        raw = open(path, "rb").read()
        if name.endswith(".html"):
            inject = "<script>window.__Q=%s;" % json.dumps(query)
            if base == "one":
                css, kind = one_css(query)
                raw = raw.replace(b"/*ONECSS*/", css.encode())
                inject += "window.__KIND=%s;window.__CSS=%s;" % (json.dumps(kind), json.dumps(css))
            inject += "</script>"
            raw = raw.replace(b"<head>", b"<head>" + inject.encode(), 1)
        self.send_response(200)
        self.send_header("Content-Type", "text/html" if name.endswith(".html") else "application/javascript")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


def start_server():
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv.server_port


# ---------------------------------------------------------------- image readers
class Img:
    """RGB bytes + width/height, from a P6 PPM (QEMU) or a PNG (Chrome)."""

    def __init__(self, px, w, h, bpp=3):
        self.px, self.w, self.h, self.bpp = px, w, h, bpp

    @staticmethod
    def ppm(path):
        data = open(path, "rb").read()
        m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
        if not m:
            raise ValueError(path + ": not a P6 PPM")
        return Img(data[m.end():], int(m.group(1)), int(m.group(2)), 3)

    @staticmethod
    def png(data):
        p = 8
        w = h = ct = 0
        idat = b""
        while p < len(data):
            l = struct.unpack(">I", data[p:p + 4])[0]
            t = data[p + 4:p + 8]
            c = data[p + 8:p + 8 + l]
            p += 12 + l
            if t == b"IHDR":
                w, h, _bd, ct = struct.unpack(">IIBB", c[:10])
            elif t == b"IDAT":
                idat += c
        raw = zlib.decompress(idat)
        bpp = 4 if ct == 6 else 3
        stride = w * bpp
        out = bytearray()
        prev = bytearray(stride)
        q = 0
        for _y in range(h):
            f = raw[q]
            q += 1
            line = bytearray(raw[q:q + stride])
            q += stride
            if f == 1:
                for i in range(bpp, stride):
                    line[i] = (line[i] + line[i - bpp]) & 255
            elif f == 2:
                for i in range(stride):
                    line[i] = (line[i] + prev[i]) & 255
            elif f == 3:
                for i in range(stride):
                    a = line[i - bpp] if i >= bpp else 0
                    line[i] = (line[i] + (a + prev[i]) // 2) & 255
            elif f == 4:
                for i in range(stride):
                    a = line[i - bpp] if i >= bpp else 0
                    b = prev[i]
                    c = prev[i - bpp] if i >= bpp else 0
                    pp = a + b - c
                    pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                    line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            out += line
            prev = line
        return Img(bytes(out), w, h, bpp)

    def at(self, x, y):
        if x < 0 or y < 0 or x >= self.w or y >= self.h:
            return None
        o = (y * self.w + x) * self.bpp
        return (self.px[o], self.px[o + 1], self.px[o + 2])

    def bbox(self, rgb):
        """Bounding box of every pixel EXACTLY equal to rgb, or None."""
        target = bytes(rgb)
        x0 = y0 = 1 << 30
        x1 = y1 = -1
        row = self.w * self.bpp
        for y in range(self.h):
            base = y * row
            start = 0
            while True:
                k = self.px.find(target, base + start, base + row)
                if k < 0:
                    break
                off = k - base
                if off % self.bpp:
                    start = off + 1
                    continue
                x = off // self.bpp
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
                start = off + self.bpp
        if x1 < 0:
            return None
        return (x0, y0, x1, y1)

    def dark_bbox(self, x0, y0, x1, y1, thresh=90):
        bx0 = by0 = 1 << 30
        bx1 = by1 = -1
        n = 0
        for y in range(max(0, y0), min(self.h, y1)):
            for x in range(max(0, x0), min(self.w, x1)):
                o = (y * self.w + x) * self.bpp
                if self.px[o] < thresh and self.px[o + 1] < thresh and self.px[o + 2] < thresh:
                    n += 1
                    if x < bx0: bx0 = x
                    if x > bx1: bx1 = x
                    if y < by0: by0 = y
                    if y > by1: by1 = y
        return (n, (bx0, by0, bx1, by1) if n else None)


def diffcount(a, b, y_min):
    n = 0
    row = a.w * 3
    pa, pb = a.px, b.px
    for y in range(y_min, a.h):
        base = y * row
        for o in range(base, base + row, 3):
            if pa[o] != pb[o] or pa[o + 1] != pb[o + 1] or pa[o + 2] != pb[o + 2]:
                n += 1
    return n


# ---------------------------------------------------------------- measurement
def measure(img, boxes):
    """Every box's position/size/alpha/colour relative to the anchor, plus the
    stamp. Returns a dict; 'stamp_ms' is None when the bar has not moved."""
    r = {"anchor": None, "stamp_ms": None, "boxes": {}}
    a = img.bbox(ANCHOR)
    if not a:
        return r
    ox, oy = a[0] - 10, a[1] - 10
    r["anchor"] = [ox, oy]
    bar = img.bbox(CLKBAR)
    if bar and bar[2] > bar[0]:
        r["stamp_ms"] = (bar[2] - bar[0] + 1) * 5
    for b in boxes:
        rgb = tuple(b["rgb"])
        bw, bh = b.get("w", 60), b.get("h", 60)
        m = {}
        bb = img.bbox(rgb)
        if bb:
            m["x"] = bb[0] - ox
            m["y"] = bb[1] - oy
            m["w"] = bb[2] - bb[0] + 1
            m["h"] = bb[3] - bb[1] + 1
        else:
            m["x"] = None
        # centre pixel of the DECLARED box: alpha (over white) and colour
        cx, cy = ox + b["x"] + bw // 2, oy + b["y"] + bh // 2
        p = img.at(cx, cy)
        if p:
            m["centre"] = list(p)
            k = max(range(3), key=lambda i: 255 - rgb[i])
            den = 255 - rgb[k]
            m["alpha"] = round((255 - p[k]) / float(den), 3) if den > 0 else None
        if b["kind"].startswith("rot"):
            n, db = img.dark_bbox(ox + b["x"] - 70, oy + b["y"] - 70, ox + b["x"] + bw + 70, oy + b["y"] + bh + 70)
            m["dark_px"] = n
            m["dark_bbox"] = [db[0] - ox, db[1] - oy, db[2] - db[0] + 1, db[3] - db[1] + 1] if db else None
        r["boxes"][b["id"]] = m
    return r


# ---------------------------------------------------------------- analytic oracle
def bezier_y(x, p1x, p1y, p2x, p2y):
    lo, hi = 0.0, 1.0
    for _ in range(40):
        mid = (lo + hi) / 2
        bx = 3 * (1 - mid) ** 2 * mid * p1x + 3 * (1 - mid) * mid ** 2 * p2x + mid ** 3
        if bx < x: lo = mid
        else: hi = mid
    t = (lo + hi) / 2
    return 3 * (1 - t) ** 2 * t * p1y + 3 * (1 - t) * t ** 2 * p2y + t ** 3


def expect(kind, t, base_x):
    """What a conforming engine shows at t ms after the trigger/start.
    Returns a dict of the expected observables (x, alpha, w/h, colour)."""
    if t is None:
        return {}
    clamp = lambda v: max(0.0, min(1.0, v))
    p = clamp(t / 1000.0)
    e = {}
    kinds = kind.split("+")
    for k in kinds:
        if k in ("xf_lin", "xf_lin_fwd", "js_xf_100", "left_lin", "left_lin_fwd"):
            e["x"] = base_x + (t / 10.0 if k == "js_xf_100" else 200 * p)
        elif k == "js_left_100":
            e["x"] = base_x + t / 10.0
        elif k == "xf_eio":
            e["x"] = base_x + 200 * bezier_y(p, 0.42, 0, 0.58, 1)
        elif k in ("xf_delay500", "xf_delay500_fwd"):
            e["x"] = base_x + 200 * clamp((t - 500) / 1000.0)
        elif k == "xf_steps4":
            e["x"] = base_x + 200 * (min(4, int(p * 4)) / 4.0 if t < 1000 else 1.0)
        elif k == "xf_hold_jump":
            e["x"] = base_x + (0 if t < 1000 else 200)
        elif k == "xf_tri_inf":
            f = (t / 1000.0) % 1.0
            e["x"] = base_x + 200 * (f * 2 if f < 0.5 else (1 - f) * 2)
        elif k == "xf_seek500":
            e["x"] = base_x + 100
        elif k == "never":
            e["x"] = base_x
        elif k == "op_lin":
            e["alpha"] = 0.2 + 0.8 * p
        elif k == "op_lin_0_1":
            e["alpha"] = p
        elif k == "js_op_1s":
            e["alpha"] = p
        elif k == "bg_lin" or k == "bg_lin_fwd":
            e["centre"] = [round(201 + (1 - 201) * p), round(1 + (201 - 1) * p), 254]
        elif k == "rot_lin_inf":
            import math
            ang = (t / 1000.0 % 1.0) * 2 * math.pi
            c, s = abs(math.cos(ang)), abs(math.sin(ang))
            e["w"] = round(120 * c + 40 * s)
            e["h"] = round(120 * s + 40 * c)
        elif k == "rot90_lin_inf":
            import math
            ang = (t / 1000.0 % 1.0) * (math.pi / 2)
            c, s_ = abs(math.cos(ang)), abs(math.sin(ang))
            e["w"] = round(120 * c + 40 * s_)
            e["h"] = round(120 * s_ + 40 * c)
        elif k == "scale_lin_fwd":
            e["w"] = e["h"] = round(60 * (0.5 + 0.5 * p))
    return e


def fmt_row(page, shot, meas, boxes, t0_ms, stamp_note):
    """One printed line per box, plus the header for the shot."""
    st = meas["stamp_ms"]
    rel = (st - t0_ms) if (st is not None and t0_ms is not None) else None
    print("  shot %-6s stamp=%s ms  rel=%s ms  anchor=%s  %s" % (
        shot, st, None if rel is None else int(rel), meas["anchor"], stamp_note))
    for b in boxes:
        m = meas["boxes"].get(b["id"], {})
        ex = expect(b["kind"], rel, b["x"])
        got = "x=%s" % m.get("x")
        if "w" in m and (b["kind"].startswith("rot") or b["kind"].startswith("scale")):
            got += " wh=%sx%s" % (m.get("w"), m.get("h"))
        if "alpha" in m:
            got += " a=%s" % m.get("alpha")
        if b["kind"].startswith("bg"):
            got += " c=%s" % m.get("centre")
        if "dark_px" in m:
            got += " dark=%s@%s" % (m["dark_px"], m["dark_bbox"])
        exs = " ".join("%s=%s" % (k, (round(v, 2) if isinstance(v, float) else v)) for k, v in ex.items())
        print("    %-4s %-34s got %-40s expect %s" % (b["id"], b["why"][:34], got, exs))


# ---------------------------------------------------------------- guest run
def guest_run(iso, disk, outdir, only):
    from qmp_ui import Session, configure, pt   # noqa: E402

    port = start_server()
    os.makedirs(outdir, exist_ok=True)
    serial_path = os.path.join(outdir, "serial.log")
    qmp_path = os.path.join(outdir, "qmp-%d.sock" % os.getpid())
    for p in (serial_path, qmp_path):
        try: os.unlink(p)
        except OSError: pass
    XRES, YRES = 1280, 800
    configure(XRES, YRES)
    proc = subprocess.Popen(
        [os.environ.get("QEMU", "qemu-system-x86_64"), "-cpu", os.environ.get("QEMU_CPU", "max"),
         "-cdrom", iso, "-drive", "file=%s,format=raw,if=none,id=hd0" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot", "-m", "512M",
         "-smp", "4", "-accel", "tcg,thread=multi", "-vga", "none",
         "-device", "virtio-gpu-pci,xres=%d,yres=%d" % (XRES, YRES), "-display", "none",
         "-no-reboot", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-serial", "file:" + serial_path, "-qmp", "unix:%s,server,nowait" % qmp_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    # A serial watcher with HOST arrival times per line, so the guest-vs-host
    # clock rate can be read off the ANIM-*-MARK lines afterwards.
    arrivals = []          # (host_time, line)
    seen = [0]
    lock = threading.Lock()
    stop = [False]

    def watch():
        pos = 0
        buf = b""
        while not stop[0]:
            try:
                with open(serial_path, "rb") as fh:
                    fh.seek(pos)
                    chunk = fh.read()
            except OSError:
                chunk = b""
            if chunk:
                pos += len(chunk)
                buf += chunk
                now = time.time()
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    with lock:
                        arrivals.append((now, line.decode("utf-8", "replace")))
            time.sleep(0.02)

    threading.Thread(target=watch, daemon=True).start()

    def wait_line(needle, secs, since=0):
        end = time.time() + secs
        while time.time() < end:
            with lock:
                for i in range(since, len(arrivals)):
                    if needle in arrivals[i][1]:
                        return i
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited while waiting for " + needle)
            time.sleep(0.02)
        return -1

    def mark():
        with lock:
            return len(arrivals)

    def lines_since(since):
        with lock:
            return list(arrivals[since:])

    result = {"mode": "guest", "iso": iso, "disk": disk, "pages": {}}
    try:
        if wait_line("LOGIT_BOOT_OK", 240) < 0:
            raise RuntimeError("kernel never printed LOGIT_BOOT_OK")
        time.sleep(6)
        ui = Session(qmp_path, serial=serial_path)
        ui.launch_app("browser")
        time.sleep(3.0)

        def shot(name):
            path = os.path.join(outdir, name + ".ppm")
            t_host = time.time()
            ui.cmd({"execute": "screendump", "arguments": {"filename": path}})
            return path, t_host

        first = True
        specs = only if only else PAGES
        for spec in specs:
            page, _q, query = spec.partition("?")
            quiet = "quiet=1" in query
            P = PREFIX[page]
            since = mark()
            ui.click_at(pt(420), pt(145 if first else 175))
            first = False
            for _ in range(80):
                ui.key("backspace")
            ui.typ("http://10.0.2.2:%d/%s" % (port, spec_path(spec)))
            ui.key("ret")
            i = wait_line("ANIM-%s-READY" % P, 120, since)
            if i < 0:
                print("PAGE %s: never printed READY -- skipped" % spec)
                result["pages"][spec] = {"error": "no READY"}
                continue
            pr = {"shots": {}, "log": [], "spec": spec}
            key = spec.replace("?", "_").replace("&", "_").replace("=", "")
            # the box table, from the page
            bi = wait_line("ANIM-%s-BOXES" % P, 5, since)
            boxes = json.loads(arrivals[bi][1].split("ANIM-%s-BOXES " % P, 1)[1]) if bi >= 0 else []
            pr["boxes"] = boxes
            shots = []
            trig = TRIGGERED[page]
            # t~0 shot right at READY (for a triggered page this is the PRE state)
            shots.append(("PRE" if trig else "T0",) + shot("%s_%s" % (key, "pre" if trig else "t0")))
            if quiet:
                # QUIET: nothing prints between GO/READY and DONE, so the shots
                # are scheduled from the HOST clock relative to the marker's
                # host arrival (guest/host clock rate measured ~0.99 on the
                # non-quiet pages of the same run).
                if trig:
                    j = wait_line("ANIM-%s-GO " % P, 30, since)
                    shots.append(("GO",) + shot("%s_go" % key))
                    t_go = arrivals[j][0]
                else:
                    t_go = arrivals[i][0]
                pr["t_go_host"] = t_go
                for d in (250, 600, 1200, 2500):
                    while time.time() < t_go + d / 1000.0:
                        time.sleep(0.005)
                    shots.append(("H%d" % d,) + shot("%s_h%d" % (key, d)))
                wait_line("ANIM-%s-DONE" % P, 15, since)
            else:
                marks = (["GO"] if trig else []) + ["T250", "T600", "T1200", "T2500"]
                for mk in marks:
                    j = wait_line("ANIM-%s-%s " % (P, mk), 30, since)
                    if j < 0:
                        print("  page %s: marker %s never came" % (spec, mk))
                        continue
                    shots.append((mk,) + shot("%s_%s" % (key, mk.lower())))
                # let the page finish its MARK lines (6 s) and DONE
                wait_line("ANIM-%s-MARK i=6" % P, 12, since)
            pr["log"] = [l for _t, l in lines_since(since) if "ANIM-" in l]
            # T0 of the page's clock: the GO line for triggered pages, READY otherwise
            t0_ms = None
            for l in pr["log"]:
                m = re.search(r"ANIM-%s-%s t=(\d+)" % (P, "GO" if trig else "READY"), l)
                if m:
                    t0_ms = int(m.group(1))
            pr["t0_ms"] = t0_ms
            # clock rate: guest ms per host s from the MARK lines
            pts = []
            for th, l in lines_since(since):
                m = re.search(r"ANIM-%s-MARK i=(\d+) t=(\d+)" % P, l)
                if m:
                    pts.append((th, int(m.group(2))))
            if len(pts) >= 3:
                (h0, g0), (h1, g1) = pts[0], pts[-1]
                rate = (g1 - g0) / ((h1 - h0) * 1000.0) if h1 > h0 else None
                pr["clock_guest_per_host"] = rate
            print("PAGE %s: t0=%s ms, %d shots, clock guest/host=%s" % (
                spec, t0_ms, len(shots), pr.get("clock_guest_per_host")))
            prev = None
            for name, path, th in shots:
                img = Img.ppm(path)
                meas = measure(img, boxes)
                note = ""
                if meas["stamp_ms"] is None and pr.get("t_go_host"):
                    # no stamp (nostamp/quiet): host time since the marker arrived
                    hrel = int((th - pr["t_go_host"]) * 1000)
                    meas["stamp_ms"] = (t0_ms or 0) + hrel
                    note = "(stamp = host time since marker, %d ms)" % hrel
                elif meas["stamp_ms"] is None:
                    note = "(no stamp: bar did not move)"
                if prev is not None:
                    meas["diff_px_vs_prev"] = diffcount(prev, img, 40)
                    note += " diff-vs-prev=%d px" % meas["diff_px_vs_prev"]
                prev = img
                meas["host_t"] = th
                pr["shots"][name] = meas
                fmt_row(page, name, meas, boxes, t0_ms, note)
            for l in pr["log"]:
                if re.search(r"ANIM-%s-(CS|MARK|DONE|HAS|FINISHED|ONFINISH|ERR|CSS|T\d+ )" % P, l):
                    print("    " + l.strip()[:200])
            result["pages"][spec] = pr
    finally:
        stop[0] = True
        try:
            proc.terminate()
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
    with open(os.path.join(outdir, "result.json"), "w") as fh:
        json.dump(result, fh, indent=1)
    print("result: %s" % os.path.join(outdir, "result.json"))
    return result


# ---------------------------------------------------------------- Chrome oracle (CDP)
class WS:
    """The smallest websocket client that can talk to Chrome's DevTools."""

    def __init__(self, url):
        m = re.match(r"ws://([^:/]+):(\d+)(/.*)", url)
        self.s = socket.create_connection((m.group(1), int(m.group(2))))
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(("GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                        % (m.group(3), m.group(1), m.group(2), key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.s.recv(4096)
        self.rest = buf.split(b"\r\n\r\n", 1)[1]
        self.id = 0

    def send(self, obj):
        data = json.dumps(obj).encode()
        hdr = bytearray([0x81])
        n = len(data)
        if n < 126: hdr.append(0x80 | n)
        elif n < 65536: hdr += bytes([0x80 | 126]) + struct.pack(">H", n)
        else: hdr += bytes([0x80 | 127]) + struct.pack(">Q", n)
        mask = os.urandom(4)
        hdr += mask
        self.s.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def _read(self, n):
        while len(self.rest) < n:
            chunk = self.s.recv(1 << 20)
            if not chunk:
                raise RuntimeError("websocket closed")
            self.rest += chunk
        out, self.rest = self.rest[:n], self.rest[n:]
        return out

    def recv(self):
        while True:
            h = self._read(2)
            op = h[0] & 0x0F
            n = h[1] & 0x7F
            if n == 126: n = struct.unpack(">H", self._read(2))[0]
            elif n == 127: n = struct.unpack(">Q", self._read(8))[0]
            if h[1] & 0x80:
                self._read(4)
            data = self._read(n)
            if op == 1:
                return json.loads(data.decode())
            if op == 8:
                raise RuntimeError("websocket closed by peer")

    def call(self, method, params=None, timeout=30):
        self.id += 1
        mid = self.id
        self.send({"id": mid, "method": method, "params": params or {}})
        end = time.time() + timeout
        while time.time() < end:
            msg = self.recv()
            if msg.get("id") == mid:
                return msg.get("result", msg)
        raise RuntimeError("CDP timeout: " + method)


def oracle_run(outdir, only):
    chrome = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
    if not os.path.exists(chrome):
        print("SKIP: no headless Chrome at %s -- the oracle needs it" % chrome)
        return None
    port = start_server()
    os.makedirs(outdir, exist_ok=True)
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    dport = s.getsockname()[1]
    s.close()
    udd = tempfile.mkdtemp(prefix="atk_anim_chrome_")
    proc = subprocess.Popen([chrome, "--headless=new", "--disable-gpu", "--hide-scrollbars",
                             "--window-size=1280,800", "--remote-debugging-port=%d" % dport,
                             "--user-data-dir=" + udd, "--no-first-run", "about:blank"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    result = {"mode": "chrome", "pages": {}}
    try:
        ws = None
        for _ in range(100):
            try:
                lst = json.loads(urllib.request.urlopen("http://127.0.0.1:%d/json/list" % dport).read())
                pg = [t for t in lst if t.get("type") == "page"]
                if pg:
                    ws = WS(pg[0]["webSocketDebuggerUrl"])
                    break
            except Exception:
                pass
            time.sleep(0.2)
        if not ws:
            raise RuntimeError("Chrome never exposed a page target")
        ws.call("Page.enable")
        ws.call("Runtime.enable")
        specs = only if only else PAGES
        for spec in specs:
            page, _q, query = spec.partition("?")
            P = PREFIX[page]
            trig = TRIGGERED[page]
            url = "http://127.0.0.1:%d/%s" % (port, spec_path(spec))
            pr = {"shots": {}, "spec": spec}
            ws.call("Page.navigate", {"url": url})
            shots = []
            if trig:
                time.sleep(0.9)          # before T0=1500 ms
                shots.append(("PRE", ws.call("Page.captureScreenshot", {"format": "png"})["data"]))
                time.sleep(0.9)          # ~ T0+300
                shots.append(("~T250", ws.call("Page.captureScreenshot", {"format": "png"})["data"]))
            else:
                time.sleep(0.35)
                shots.append(("~T250", ws.call("Page.captureScreenshot", {"format": "png"})["data"]))
            time.sleep(5.5)
            shots.append(("END", ws.call("Page.captureScreenshot", {"format": "png"})["data"]))
            time.sleep(1.5)
            log = ws.call("Runtime.evaluate", {"expression": "window.__log.join('\\n')", "returnByValue": True})
            log = log.get("result", {}).get("value", "")
            pr["log"] = log.split("\n")
            boxes = []
            t0_ms = None
            for l in pr["log"]:
                if l.startswith("ANIM-%s-BOXES " % P):
                    boxes = json.loads(l.split(" ", 1)[1])
                m = re.search(r"ANIM-%s-%s t=(\d+)" % (P, "GO" if trig else "READY"), l)
                if m:
                    t0_ms = int(m.group(1))
            pr["boxes"] = boxes
            pr["t0_ms"] = t0_ms
            print("PAGE %s (chrome): t0=%s ms" % (spec, t0_ms))
            for name, b64 in shots:
                data = base64.b64decode(b64)
                key = spec.replace("?", "_").replace("&", "_").replace("=", "")
                with open(os.path.join(outdir, "chrome_%s_%s.png" % (key, name.strip("~").lower())), "wb") as fh:
                    fh.write(data)
                img = Img.png(data)
                meas = measure(img, boxes)
                pr["shots"][name] = meas
                fmt_row(page, name, meas, boxes, t0_ms, "")
            for l in pr["log"]:
                if re.search(r"ANIM-%s-(CS|MARK|DONE|HAS|FINISHED|ONFINISH|ERR|CSS|T\d+ )" % P, l):
                    print("    " + l.strip()[:200])
            result["pages"][spec] = pr
    finally:
        try:
            proc.terminate()
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
    with open(os.path.join(outdir, "result.json"), "w") as fh:
        json.dump(result, fh, indent=1)
    print("result: %s" % os.path.join(outdir, "result.json"))
    return result


def main():
    args = sys.argv[1:]
    outdir = None
    only = None
    oracle = False
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--out": outdir = args[i + 1]; i += 2
        elif args[i] == "--only": only = args[i + 1].split(","); i += 2
        elif args[i] == "--oracle": oracle = True; i += 1
        else: pos.append(args[i]); i += 1
    if oracle:
        oracle_run(outdir or tempfile.mkdtemp(prefix="atk_anim_oracle_"), only)
        return 0
    if len(pos) < 2:
        print(__doc__)
        return 2
    guest_run(pos[0], pos[1], outdir or tempfile.mkdtemp(prefix="atk_anim_guest_"), only)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
