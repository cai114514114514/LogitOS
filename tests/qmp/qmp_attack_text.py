#!/usr/bin/env python3
"""Text-alignment differential: LogitOS's browser against real Chrome, same
font bytes, same viewport width, per element: x, y, w, h, baseline.

    python3 tests/qmp/qmp_attack_text.py <iso> <disk.img> --out DIR   # guest half
    python3 tests/qmp/qmp_attack_text.py --chrome --out DIR             # Chrome half
    python3 tests/qmp/qmp_attack_text.py --diff DIR                     # the table

Owner's symptom: "text is misaligned". This turns the sentence into numbers.

THE ORACLE IS HEADLESS CHROME ON THE HOST, fed the SAME .ttf files the guest
draws with (the fixture's @font-face rules point at /fonts/, which this
server maps onto fsroot/fonts/). That is what separates font-metric noise
from engine mistakes: both engines measure Noto Sans SC subset glyphs with
hhea 1160/-288 and the mono face with 1069/-293, so a rect that differs is a
LAYOUT difference, not a "Chrome has Helvetica" difference. Chrome runs at
--force-device-scale-factor=1 and --window-size=<guest viewport width>,
where the guest width is what browser.c's pick_born_size() gives a 1280x800
desktop: 1280*88/100 = 1126 pt, at backing scale 100 (fb.c pick_scale), so a
point IS a device pixel on both sides and no number is rescaled.

WHAT IS READ FROM THE GUEST. Three things, and they cross-check each other:
  1. the page's own TXM/TXW console lines (getBoundingClientRect per id) --
     js_dom.c reads these off the display list, so they are what layout
     PRODUCED;
  2. the browser's [dl] painted-text dump (x,y per painted run, window
     coordinates; y - VIEW_Y is the document y) -- what the painter DREW,
     which is where the word positions and line breaks come from because
     the guest has no Range;
  3. the screendump, from which the ink baseline of a no-descender line is
     measured directly, because the guest's baseline is not in any API: the
     kernel adds hhea.ascent*px/upem to the run's top itself (text.c
     ascent_px). The diff derives the guest baseline as run_top + floor(
     1160*px/1000) and the ink measurement is the check on that model.

The fixture's baseline probe (a 0x0 inline-block appended to the element)
is a Chrome measurement only: in the guest an inline-block is a block and a
rect is read from the last completed layout, so it reports 0 there. That is
recorded, not papered over.

Nothing here asserts host wall-clock; the guest is waited on by serial
markers. The QMP socket lives under --out so several agents' QEMUs never
share a path (a stale qmp.sock in a shared cwd reads as a dead guest).
"""

import html
import os
import re
import subprocess
import sys
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FIXDIR = os.path.join(ROOT, "tests", "fixtures", "attack", "text")
FONTDIR = os.path.join(ROOT, "fsroot", "fonts")
PAGES = ("text1.html", "text2.html", "text3.html")
PAGE_IDS = ("p1", "p2", "p3")

# browser.c: born size = screen*88/100 x screen*80/100 on a 1280x800 pt desktop
# (pick_born_size); VIEW_Y = TABH + BARH = 60; the layout width is win_w.
GUEST_W_PT = 1280 * 88 // 100          # 1126
GUEST_H_PT = 800 * 80 // 100           # 640
VIEW_Y_PT = 60
# c/kernel/gui/text.c ascent_px(): baseline = top + ascent*px/upem, integer
# division. hhea.ascent of fsroot/fonts/ui.ttf is 1160/1000, mono.ttf 1069/1000
# (read off the font bytes by the analysis that built this driver).
ASC_UI, ASC_MONO = 1160, 1069

CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"


# ---------------------------------------------------------------- server ----
class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.0"

    def do_GET(self):
        path = self.path.split("?", 1)[0]
        if path.startswith("/fonts/"):
            fn = os.path.join(FONTDIR, os.path.basename(path))
            ctype = "font/ttf"
        else:
            fn = os.path.join(FIXDIR, os.path.basename(path) or "text1.html")
            ctype = "text/html; charset=utf-8"
        try:
            with open(fn, "rb") as fh:
                raw = fh.read()
        except OSError:
            self.send_response(404)
            self.end_headers()
            return
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


def serve():
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_port


# ---------------------------------------------------------------- chrome ----
def chrome_half(out):
    if not os.path.exists(CHROME):
        print("SKIP: no Chrome at %s -- the oracle is absent; install Google Chrome" % CHROME)
        sys.exit(2)
    srv, port = serve()
    os.makedirs(out, exist_ok=True)
    for page, pid in zip(PAGES, PAGE_IDS):
        url = "http://127.0.0.1:%d/%s" % (port, page)
        base = [CHROME, "--headless=new", "--disable-gpu", "--no-sandbox", "--hide-scrollbars",
                "--force-device-scale-factor=1", "--window-size=%d,%d" % (GUEST_W_PT, 900),
                "--virtual-time-budget=8000"]
        # No --user-data-dir: with one, Chrome 151 headless=new sat past a 120 s
        # timeout on this page (measured twice, http and file); without it the
        # same command returns in ~2 s. The default headless profile is a temp
        # directory Chrome discards, which is what a measurement wants anyway.
        dom = subprocess.run(base + ["--dump-dom", url], capture_output=True, text=True, timeout=120).stdout
        m = re.search(r'<pre id="txm-out">(.*?)</pre>', dom, re.S)
        if not m:
            print("FAIL: Chrome's DOM dump for %s carries no #txm-out -- the fixture's script did not run" % page)
            sys.exit(1)
        lines = html.unescape(m.group(1))
        with open(os.path.join(out, "chrome_%s.txt" % pid), "w") as fh:
            fh.write(lines + "\n")
        png = os.path.join(out, "chrome_%s.png" % pid)
        subprocess.run(base + ["--screenshot=" + png, url], capture_output=True, text=True, timeout=120)
        n = len([l for l in lines.splitlines() if l.startswith("TXM " + pid + " t1 ")])
        print("chrome %s: %d TXM t1 lines, screenshot %s" % (pid, n, png))
    srv.shutdown()


# ----------------------------------------------------------------- guest ----
def guest_half(iso, disk, out):
    from qmp_ui import Session, PPM   # noqa: E402
    import qmp_addrbar                # noqa: E402
    os.makedirs(out, exist_ok=True)
    qmp_path = os.path.join(out, "qmp-%d.sock" % os.getpid())
    serial_path = os.path.join(out, "serial.log")
    for p in (qmp_path, serial_path):
        try:
            os.unlink(p)
        except OSError:
            pass
    srv, port = serve()
    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    proc = subprocess.Popen(
        [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0" % disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
         "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
         "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
         "-display", "none", "-no-reboot",
         "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
         "-serial", "file:" + serial_path,
         "-qmp", "unix:%s,server,nowait" % qmp_path],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def serial():
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def die(msg):
        print("FAIL: " + msg)
        print("----- serial (tail) -----")
        print(serial()[-3000:])
        proc.kill()
        sys.exit(1)

    def wait_serial(needle, secs, what, start=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial()[start:]:
                return True
            if proc.poll() is not None:
                die("QEMU exited while waiting for " + what)
            time.sleep(0.5)
        return False

    try:
        if not wait_serial("LOGIT_BOOT_OK", 300, "boot"):
            die("kernel never printed LOGIT_BOOT_OK")
        if not wait_serial("desktop live", 120, "desktop"):
            die("no 'desktop live'")
        time.sleep(4)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            die(str(e))
        time.sleep(8.0)
        origin = None
        for page, pid in zip(PAGES, PAGE_IDS):
            mark = len(serial())
            if origin is None:
                # The ~3 MB browser.aex paints its chrome seconds after the
                # [wm] launched line, and on a contended host (several QEMUs)
                # it once took longer than qmp_addrbar's 12 s caret poll: the
                # window was up, blank, and the caret not yet drawn. One more
                # poll before giving up is the difference between a flake and
                # a finding.
                try:
                    origin = qmp_addrbar.focus(ui)
                except SystemExit:
                    print("note: caret not found on the first poll -- waiting 15 s and polling once more")
                    time.sleep(15.0)
                    origin = qmp_addrbar.focus(ui)
            else:
                qmp_addrbar.focus(ui, origin)
                ui.key("end")
                for _ in range(90):
                    ui.key("backspace", settle=0.02)
            ui.typ("http://10.0.2.2:%d/%s" % (port, page))
            qmp_addrbar.typed_echo(ui, origin)
            ui.key("ret")
            if not wait_serial("TXM %s t1 END" % pid, 180, "page %s t1" % pid, start=mark):
                die("page %s never printed its t1 END line" % pid)
            time.sleep(3.0)                  # settle: relayout after the probe removal, repaint
            shot = os.path.join(out, "guest_%s.ppm" % pid)
            ui.screendump(shot, settle=0.5)
            seg = serial()[mark:]
            with open(os.path.join(out, "guest_%s.txt" % pid), "w") as fh:
                fh.write("ORIGIN %d %d\n" % origin)
                fh.write(seg)
            try:
                from PIL import Image
                Image.open(shot).save(os.path.join(out, "guest_%s.png" % pid))
            except Exception as e:  # PIL is a convenience for the report, not the measurement
                print("note: no PNG for %s (%s)" % (shot, e))
            n = len(re.findall(r"^TXM %s t1 " % pid, seg, re.M))
            print("guest %s: %d TXM t1 lines, screendump %s, client origin %r" % (pid, n, shot, origin))
        with open(os.path.join(out, "serial_full.log"), "w") as fh:
            fh.write(serial())
    finally:
        proc.kill()
        srv.shutdown()


# ------------------------------------------------------------------ diff ----
TXM_RE = re.compile(r"^TXM (p\w+) (t\d) (\S+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) base=(\S+)", re.M)
TXW_RE = re.compile(r"^TXW (p\w+) (t\d) (\S+) (\d+) (\S+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+) (-?[\d.]+)", re.M)
DL_RE = re.compile(r"^\[dl\] (-?\d+),(-?\d+) (.*)$", re.M)


def parse_txm(text, phase="t1"):
    d = {}
    for m in TXM_RE.finditer(text):
        if m.group(2) != phase:
            continue
        d[m.group(3)] = dict(x=float(m.group(4)), y=float(m.group(5)), w=float(m.group(6)),
                             h=float(m.group(7)), base=m.group(8))
    return d


def parse_txw(text, phase="t1"):
    d = {}
    for m in TXW_RE.finditer(text):
        if m.group(2) != phase:
            continue
        d.setdefault(m.group(3), []).append((int(m.group(4)), m.group(5), float(m.group(6)),
                                             float(m.group(7)), float(m.group(8)), float(m.group(9))))
    return d


def last_dl_dump(text):
    """The last complete [dl] painted-text dump in a serial segment, as
    [(x, doc_y, text)] with y already moved from window to document space."""
    chunks = text.split("[dl] ---8<--- begin painted text")[1:]
    for ch in reversed(chunks):
        if "[dl] ---8<--- end painted text" not in ch:
            continue
        ch = ch.split("[dl] ---8<--- end painted text")[0]
        runs = [(int(m.group(1)), int(m.group(2)) - VIEW_Y_PT, m.group(3)) for m in DL_RE.finditer(ch)]
        if runs:
            return runs
    return []


def ink_bottom(img, x0, y0, x1, y1, thresh=110):
    """Lowest row in [y0,y1) x [x0,x1) holding a pixel darker than thresh; -1 if none.
    `img` is a PIL RGB image; coordinates are device pixels."""
    px = img.load()
    W, H = img.size
    for y in range(min(y1, H) - 1, max(y0, 0) - 1, -1):
        for x in range(max(x0, 0), min(x1, W)):
            r, g, b = px[x, y][:3]
            if (r * 299 + g * 587 + b * 114) // 1000 < thresh:
                return y
    return -1


def ink_box(img, x0, y0, x1, y1, thresh=110):
    px = img.load()
    W, H = img.size
    bx0 = by0 = 1 << 30
    bx1 = by1 = -1
    for y in range(max(y0, 0), min(y1, H)):
        for x in range(max(x0, 0), min(x1, W)):
            r, g, b = px[x, y][:3]
            if (r * 299 + g * 587 + b * 114) // 1000 < thresh:
                bx0 = min(bx0, x); bx1 = max(bx1, x); by0 = min(by0, y); by1 = max(by1, y)
    return None if bx1 < 0 else (bx0, by0, bx1 + 1, by1 + 1)


def fmt(v):
    return ("%+.1f" % v) if abs(v) >= 0.05 else "0"


def diff_half(out):
    from PIL import Image
    for pid in PAGE_IDS:
        gp = os.path.join(out, "guest_%s.txt" % pid)
        cp = os.path.join(out, "chrome_%s.txt" % pid)
        if not (os.path.exists(gp) and os.path.exists(cp)):
            print("== %s: missing %s" % (pid, gp if not os.path.exists(gp) else cp))
            continue
        gtxt, ctxt = open(gp).read(), open(cp).read()
        om = re.search(r"^ORIGIN (\d+) (\d+)", gtxt, re.M)
        ox, oy = (int(om.group(1)), int(om.group(2))) if om else (0, 0)
        g, c = parse_txm(gtxt), parse_txm(ctxt)
        g0, c0 = parse_txm(gtxt, "t0"), parse_txm(ctxt, "t0")
        gimg = Image.open(os.path.join(out, "guest_%s.png" % pid)).convert("RGB")
        cimg = Image.open(os.path.join(out, "chrome_%s.png" % pid)).convert("RGB")
        runs = last_dl_dump(gtxt)
        ven = re.search(r"^TXM %s t1 END (.*)$" % pid, gtxt, re.M)
        cen = re.search(r"^TXM %s t1 END (.*)$" % pid, ctxt, re.M)
        print("=" * 100)
        print("== %s  guest END: %s" % (pid, ven.group(1) if ven else "?"))
        print("== %s  chrome END: %s" % (pid, cen.group(1) if cen else "?"))
        print("== guest client origin (%d,%d); document (0,0) is at device (%d,%d); %d painted runs in the last [dl] dump"
              % (ox, oy, ox, oy + VIEW_Y_PT, len(runs)))
        unstable = [k for k in g if k in g0 and (g[k]["x"], g[k]["y"], g[k]["w"], g[k]["h"]) !=
                    (g0[k]["x"], g0[k]["y"], g0[k]["w"], g0[k]["h"])]
        if unstable:
            print("== guest t0 != t1 for: %s" % " ".join(unstable))
        print("%-12s | %-24s | %-24s | %-6s %-6s %-6s %-6s | %-8s %-8s %-6s | %s"
              % ("id", "guest x y w h", "chrome x y w h", "dx", "dy", "dw", "dh",
                 "g.base", "c.base", "dbase", "guest ink-bottom (device y -> doc y)"))
        for k in c:
            if k not in g:
                print("%-12s | %-24s | %s" % (k, "ABSENT", "%.1f %.1f %.1f %.1f" % (c[k]["x"], c[k]["y"], c[k]["w"], c[k]["h"])))
                continue
            a, b = g[k], c[k]
            gs = "%.0f %.0f %.0f %.0f" % (a["x"], a["y"], a["w"], a["h"])
            cs = "%.1f %.1f %.1f %.1f" % (b["x"], b["y"], b["w"], b["h"])
            # guest baseline model: first painted run inside this rect that starts at its top row
            gb = ""
            for (rx, ry, rt) in runs:
                if a["y"] - 1 <= ry <= a["y"] + a["h"] and a["x"] - 1 <= rx <= a["x"] + a["w"]:
                    gb = "run@%d" % ry
                    break
            ib = ink_bottom(gimg, int(a["x"]) + ox, int(a["y"]) + oy + VIEW_Y_PT,
                            int(a["x"] + a["w"]) + ox, int(a["y"] + a["h"]) + oy + VIEW_Y_PT + 6)
            ibs = ("%d -> %d" % (ib, ib - oy - VIEW_Y_PT + 1)) if ib >= 0 else "-"
            db = ""
            try:
                if b["base"] not in ("-", "ERR") and ib >= 0:
                    db = fmt((ib - oy - VIEW_Y_PT + 1) - float(b["base"]))
            except ValueError:
                pass
            print("%-12s | %-24s | %-24s | %-6s %-6s %-6s %-6s | %-8s %-8s %-6s | %s"
                  % (k, gs, cs, fmt(a["x"] - b["x"]), fmt(a["y"] - b["y"]), fmt(a["w"] - b["w"]),
                     fmt(a["h"] - b["h"]), (a["base"] + " " + gb).strip(), b["base"], db, ibs))
        # words: Chrome Range rects vs guest [dl] runs, grouped into lines
        cw = parse_txw(ctxt)
        for wid, words in cw.items():
            if wid not in c:
                continue
            crect = c[wid]
            lines = {}
            for (n, w, x, y, ww, hh) in words:
                lines.setdefault(round(y), []).append((x, w))
            clines = [sorted(v) for _, v in sorted(lines.items())]
            grect = g.get(wid)
            glines = {}
            if grect:
                for (rx, ry, rt) in runs:
                    if grect["y"] - 1 <= ry < grect["y"] + grect["h"] and grect["x"] - 1 <= rx <= grect["x"] + grect["w"] + 1:
                        glines.setdefault(ry, []).append((rx, rt))
            glines = [sorted(v) for _, v in sorted(glines.items())]
            print("-- %s wrap: chrome %d lines, guest %d lines (rect h %.1f vs %s)"
                  % (wid, len(clines), len(glines), crect["h"], "%.0f" % grect["h"] if grect else "?"))
            for i in range(max(len(clines), len(glines))):
                cl = " ".join(w for _, w in clines[i]) if i < len(clines) else "(none)"
                gl = " ".join(w for _, w in glines[i]) if i < len(glines) else "(none)"
                same = "same" if cl == gl else "DIFF"
                print("   L%d %s\n      chrome: %s\n      guest : %s" % (i + 1, same, cl, gl))
            if clines and glines:
                # per-word x drift on the first line
                cx = {w: x for x, w in clines[0]}
                drift = [(w, x - cx[w]) for x, w in glines[0] if w in cx]
                if drift:
                    print("   first-line word x drift (guest - chrome): %s"
                          % " ".join("%s:%+.1f" % (w, d) for w, d in drift))
        # control/button label placement from ink, both sides
        for k in ("l1", "l2", "l3", "l4"):
            if k in g and k in c:
                a, b = g[k], c[k]
                gi = ink_box(gimg, int(a["x"]) + ox + 2, int(a["y"]) + oy + VIEW_Y_PT + 2,
                             int(a["x"] + a["w"]) + ox - 2, int(a["y"] + a["h"]) + oy + VIEW_Y_PT - 2)
                ci = ink_box(cimg, int(b["x"]) + 2, int(b["y"]) + 2, int(b["x"] + b["w"]) - 2, int(b["y"] + b["h"]) - 2)
                def rel(box, r, dx, dy):
                    if not box:
                        return "no ink"
                    x0, y0, x1, y1 = box
                    return "ink [%d..%d]x[%d..%d] of box [0..%.0f]x[0..%.0f]: left-pad %d right-pad %.0f top-pad %d bottom-pad %.0f" % (
                        x0 - dx - r["x"], x1 - dx - r["x"], y0 - dy - r["y"], y1 - dy - r["y"], r["w"], r["h"],
                        x0 - dx - r["x"], r["x"] + r["w"] - (x1 - dx), y0 - dy - r["y"], r["y"] + r["h"] - (y1 - dy))
                print("-- %s label: guest  %s" % (k, rel(gi, a, ox, oy + VIEW_Y_PT)))
                print("-- %s label: chrome %s" % (k, rel(ci, b, 0, 0)))


def main():
    global PAGES, PAGE_IDS
    argv = sys.argv[1:]
    out = None
    if "--out" in argv:
        i = argv.index("--out")
        out = argv[i + 1]
        del argv[i:i + 2]
    if "--pages" in argv:
        # e.g. --pages text2b.html : the page id is the file's TXM_PAGE, which
        # by convention is 'p' + the part of the name after "text".
        i = argv.index("--pages")
        PAGES = tuple(argv[i + 1].split(","))
        PAGE_IDS = tuple("p" + p[len("text"):-len(".html")] for p in PAGES)
        del argv[i:i + 2]
    if argv and argv[0] == "--chrome":
        chrome_half(out or "build-atk-text/attack-text")
    elif argv and argv[0] == "--diff":
        diff_half(argv[1] if len(argv) > 1 else (out or "build-atk-text/attack-text"))
    elif len(argv) >= 2:
        guest_half(argv[0], argv[1], out or "build-atk-text/attack-text")
    else:
        print(__doc__)
        sys.exit(2)


if __name__ == "__main__":
    main()
