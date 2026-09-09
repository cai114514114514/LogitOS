#!/usr/bin/env python3
"""qmp_attack_click.py -- THE CLICK BATTERY: which click targets work on this
browser, which do not, and where the chain breaks. An INSTRUMENT, not a gate.

    # the guest (LogitOS under QEMU, one boot):
    python3 tests/qmp/qmp_attack_click.py --iso build-atk-click/logit.iso \
        --disk build-atk-click/disk.img --out /tmp/click-guest.json
    # the oracle (headless Chrome via playwright, SAME fixture, SAME click plan):
    python3 tests/qmp/qmp_attack_click.py --oracle --out /tmp/click-chrome.json \
        [--view 1180x542]
    # the verdict table:
    python3 tests/qmp/qmp_attack_click.py --compare /tmp/click-guest.json \
        /tmp/click-chrome.json

THE OWNER'S SYMPTOM: "clicking only works on the input box". This turns that
sentence into a table, one row per CLASS of click target, with the same
fixture (tests/fixtures/attack/click/battery.html) clicked at the same
page-derived points in two browsers. A row where Chrome logs the tag and the
guest does not is a finding; a row where both agree is not.

HOW EVERY CLICK POINT IS DERIVED (no typed-in pixels, per tools/
check-test-liveness.py rule 3). The PAGE prints every target's
getBoundingClientRect on the console (CLK-RECT lines); the driver reads them
off the serial and converts viewport px -> device px with the guest's own
geometry: the `[wm] win N frame X Y W H ... Browser` line (frame origin,
device px), the WM's 30 pt titlebar (wm.c TITLEBAR_H), and the browser's
VIEW_Y = TABH + BARH = 60 pt (browser.c). At 1280x800 the scale is 100 so a
point is a pixel (qmp_ui.pick_scale). So a rect centre (cx, cy) lands at
(fx + cx, fy + 30 + 60 + cy) -- always >= 90 px below the frame top, i.e.
never in the titlebar, never in the tab strip, never in the address bar.

THREE CHANNELS PER TARGET, because one is not enough to name the break:
  1. the page's own listener tag (CLK-<tag>) -- did the event reach JS;
  2. the document-level click listener (CLK-doc:<id>) -- WHAT the hit test
     picked, for every click that produced a click event at all;
  3. for the pixel-located targets, where the box was actually PAINTED, from a
     screendump (PPM.find_color on a colour nothing else on the screen uses)
     -- against where the page SAYS it is (CLK-RECT) and where the CSSOM's
     elementFromPoint thinks a click there lands (CLK-HIT).

APPARATUS RULES OBSERVED: every wait is on a serial marker, never on host
wall clock (five agents share this host); the pointer is confirmed with the
guest's own `[wm] ptr X Y` line before each click (it is on the cursor plane,
not in the screendump); a click that produced NO line is recorded as such
rather than retried into a false positive; the QMP socket lives under a
fresh mkdtemp so a stale socket cannot masquerade as a dead guest.

NEGATIVE CONTROL (built in, not a switch): three targets MUST NOT fire in a
correct browser -- `behind` (under a tinted overlay), `behind2` (under an
INVISIBLE overlay) and `a_prevent`'s navigation. A run where every tag
appears is a broken harness, not a perfect browser.
"""
import argparse
import http.server
import json
import os
import re
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM       # noqa: E402

FIXDIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                      "..", "fixtures", "attack", "click")
FIXDIR = os.path.abspath(FIXDIR)
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")
BOOT_BUDGET = float(os.environ.get("CLK_BOOT", 300))
LOAD_BUDGET = float(os.environ.get("CLK_LOAD", 90))
EVENT_WAIT = float(os.environ.get("CLK_EVENT", 4.0))    # a tag after a click
NEG_WAIT = float(os.environ.get("CLK_NEG", 2.5))        # "nothing" must stay nothing
PARK = (40, 780)

TITLEBAR_PT = 30      # wm.c TITLEBAR_H (points; scale 100 at 1280x800)
VIEW_Y_PT = 60        # browser.c VIEW_Y = TABH(30) + BARH(30)
POPUP_ROW = 22        # browser.c POPUP_ROW: the <select> popup's row height
WHEEL_PX = 40         # browser.c: scroll += e.wheel * 40
WHEEL_NOTCHES = 8     # 320 px, the "page scrolled 300 px" case

# Colours the fixture paints on the pixel-located targets (battery.html).
COLOUR = {"xf": (0xfe, 0x01, 0x02), "after_scroll": (0x01, 0xfe, 0x02),
          "fixed_covered": (0xfe, 0x01, 0xfe), "sc_hidden": (0x01, 0x02, 0xfe),
          "hdr": (0x10, 0x20, 0x30)}


# ---- the click plan -----------------------------------------------------------
# (step name, action, argument, what a correct browser logs -- documentation
#  only; the verdict comes from --compare against the Chrome run, never from
#  this column)
PLAN = [
    ("btn_attr",      "click", "btn_attr",      "btn_attr"),
    ("btn_listener",  "click", "btn_listener",  "btn_listener"),
    ("bubble",        "click", "bubble_inner",  "bubble_outer:bubble_inner"),
    ("noink",         "click", "noink",         "noink (a div with no bg/border/text)"),
    ("svgbtn",        "click", "svgbtn",        "svgbtn (inline <svg> child)"),
    ("mdmut",         "click", "mdmut_in",      "mdmut (mousedown replaced the child)"),
    ("order",         "click", "order",         "order:mousedown, order:mouseup, order:click"),
    ("dbl",           "dblclick", "dbl",        "dbl:click:1, dbl:click:2, dbl:dblclick"),
    ("label",         "click", "lbl",           "lbl + focus:txt"),
    ("checkbox",      "click", "chk",           "chk:true"),
    ("select",        "select", "sel",          "sel:two (row 2 of the popup)"),
    ("contenteditable", "click", "ce",          "focus:ce + ce"),
    ("transform",     "click", "xf",            "xf (at the page-reported rect)"),
    ("transform_paint", "click_colour", "xf",   "xf (at the PAINTED box)"),
    ("deferred",      "click", "deferred",      "deferred (listener from <script defer>)"),
    ("module",        "click", "modtarget",     "modtarget (listener from a module script)"),
    ("pe_none",       "click", "under_pe",      "under_pe (overlay has pointer-events:none)"),
    ("blocked",       "click", "behind",        "NOT behind -- doc:blocker only"),
    ("blocked_clear", "click", "behind2",       "NOT behind2 -- doc:blocker_clear only"),
    ("zindex",        "click", "z_high",        "z_high (z:5 declared before z:1 twin)"),
    ("inner_scroll",  "click", "sc_hidden",     "sc_hidden (container scrollTop=200)"),
    ("flex",          "click", "flex_item",     "flex_item"),
    ("grid",          "click", "grid_item",     "grid_item"),
    ("fixed_pre",     "click", "fixed_covered", "fixed_covered (before scrolling)"),
    ("bare",          "click", "bare",          "doc:bare (element with no ink)"),
    ("body",          "click_body", "body_probe", "doc:BODY (click on empty body)"),
    ("hdr_pre",       "click_hdr", "hdr",       "hdr (position:fixed header)"),
    ("bubble_text",   "click_text", "inner",    "bubble_outer:bubble_inner (clicked ON the painted word)"),
    ("a_inline",      "click", "a_inline",      "a_inline (inline <a> in a <p>, at its rect)"),
    ("a_inline_text", "click_text", "more",     "a_inline (clicked ON the painted word)"),
    ("checkbox2",     "click", "chk2",          "chk2:true (STATIC checkbox in an abspos wrapper)"),
    ("select2",       "select", "sel2",         "sel2:two (STATIC select)"),
    ("btn2",          "click", "btn2",          "btn2 (STATIC button)"),
    ("wheel",         "wheel", WHEEL_NOTCHES,   "CLK-SCROLLED + CLK-READY2"),
    ("after_scroll",  "click2", "after_scroll", "after_scroll (rect after scroll)"),
    ("after_scroll_paint", "click_colour", "after_scroll", "after_scroll (painted box after scroll)"),
    ("fixed_post",    "click2", "fixed_covered", "hdr (the target is now UNDER the fixed header)"),
    ("hdr_post",      "click_hdr", "hdr",       "hdr (still at the top after scroll)"),
]
# Navigating steps: each reloads the battery first. `addrbar` is guest-only.
NAV_PLAN = [
    ("addrbar",   "addrbar", "page2.html?via=bar", "[browser] load + CLK-PAGE2 ?via=bar"),
    ("ablock",    "click", "ablock",   "ablock + CLK-PAGE2 ?via=ablock (block inside <a>)"),
    ("a_prevent", "click", "a_prevent", "a_prevent and NO navigation (inline handler returns false)"),
    ("submit_enter", "enter", "fq2",   "focus:fq2 + submit:f2 + CLK-PAGE2 ?q2=yo&via=enter"),
    ("a_plain",   "click", "a_plain",  "CLK-PAGE2 ?via=link"),
    # LAST: on the first run this click killed the browser process
    # ([fault] app exception: general protection, rip in dispatch_event), so
    # it goes after everything else and the driver relaunches if it happens.
    ("submit_btn", "click", "fsubmit", "submit:f1:fsubmit + CLK-PAGE2 ?q=hi&via=btn"),
]
# click_text in the oracle: Chrome prints no [dl] painted-text log, so the
# word is looked up as the element that holds it and its rect is clicked.
TEXT_ELEM = {"inner": "bubble_inner", "more": "a_inline"}
DL_RE = re.compile(r"^\[dl\] (\d+),(\d+) (\S+)\s*$", re.M)


def painted_words(text):
    """The LAST '[dl] ---8<---' painted-text block: {word: (x, y)} in WINDOW
    px (browser_paint.c logs vx + item.x, vy + item.y - scroll; y is the
    line box top, VIEW_Y below the viewport top)."""
    blocks = text.split("[dl] ---8<--- begin painted text")
    if len(blocks) < 2:
        return {}
    last = blocks[-1].split("[dl] ---8<--- end painted text")[0]
    out = {}
    for m in DL_RE.finditer(last):
        out.setdefault(m.group(3), (int(m.group(1)), int(m.group(2))))
    return out

RECT_RE = re.compile(r"^CLK-RECT(2?) (\S+) (-?\d+) (-?\d+) (\d+) (\d+)\s*$", re.M)
HIT_RE = re.compile(r"^CLK-HIT(2?) (\S+) (\S+)\s*$", re.M)
VIEW_RE = re.compile(r"^CLK-VIEW(2?) (\d+) (\d+)\s*$", re.M)
SCROLLY_RE = re.compile(r"^CLK-SCROLLY(2?) (-?\d+) (-?\d+) (-?\d+)\s*$", re.M)
FRAME_RE = re.compile(r"\[wm\] win (\d+) frame (\d+) (\d+) (\d+) (\d+) content (\d+) (\d+) pt.*Browser")


def parse_report(text, suffix):
    rects, hits = {}, {}
    for m in RECT_RE.finditer(text):
        if m.group(1) == suffix:
            rects[m.group(2)] = tuple(int(v) for v in m.groups()[2:])
    for m in HIT_RE.finditer(text):
        if m.group(1) == suffix:
            hits[m.group(2)] = m.group(3)
    view = None
    for m in VIEW_RE.finditer(text):
        if m.group(1) == suffix:
            view = (int(m.group(2)), int(m.group(3)))
    sy = None
    for m in SCROLLY_RE.finditer(text):
        if m.group(1) == suffix:
            sy = tuple(int(v) for v in m.groups()[1:])
    return {"rects": rects, "hits": hits, "view": view, "scrolly": sy}


def centre(r):
    x, y, w, h = r
    return (x + w // 2, y + h // 2)


def clk_lines(text):
    return [ln.rstrip() for ln in text.splitlines()
            if ln.startswith("CLK-") or ln.startswith("[browser] load:")
            or ln.startswith("[browser] FORM-POST") or "JS exception" in ln]


class Serve(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.split("?", 1)[0].lstrip("/") or "battery.html"
        full = os.path.normpath(os.path.join(FIXDIR, path))
        if not full.startswith(FIXDIR) or not os.path.isfile(full):
            body = b"not here"
            self.send_response(404)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        with open(full, "rb") as fh:
            body = fh.read()
        ctype = ("text/html; charset=utf-8" if full.endswith(".html")
                 else "application/javascript")
        self.send_response(200)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


def start_server():
    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


# =============================================================================
# THE GUEST
# =============================================================================

def run_guest(args):
    srv = start_server()
    port = srv.server_port
    tmp = tempfile.mkdtemp(prefix="qmp_attack_click_")
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")
    rec = {"mode": "guest", "iso": args.iso, "disk": args.disk, "tmp": tmp,
           "serial_log": serial_path, "steps": [], "frame": None,
           "started": time.strftime("%Y-%m-%dT%H:%M:%S")}
    cmd = [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "1G", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    def serial():
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")
        except OSError:
            return ""

    def wait_for(needle, secs, frm=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial()[frm:]:
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.3)
        return False

    def wait_any(prefix, secs, frm):
        """True once any line starting with `prefix` appears after `frm`."""
        end = time.time() + secs
        while time.time() < end:
            for ln in serial()[frm:].splitlines():
                if ln.startswith(prefix):
                    return True
            if proc.poll() is not None:
                return False
            time.sleep(0.2)
        return False

    def finish(code, why):
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["serial_tail"] = serial()[-6000:]
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w") as fh:
            json.dump(rec, fh, indent=1)
        print(json.dumps({"why": why, "steps": len(rec["steps"])}, indent=1))
        sys.exit(code)

    def frame():
        m = None
        for ln in serial().splitlines():
            g = FRAME_RE.search(ln)
            if g:
                m = tuple(int(v) for v in g.groups())
        return m

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish(2, "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            finish(2, "the window manager never brought the desktop up")
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish(2, str(e))
        time.sleep(7)
        ui.goto(*PARK)
        fr = frame()
        if not fr:
            finish(2, "no '[wm] win N frame ... Browser' line: cannot derive the "
                      "viewport origin")
        wi, fx, fy, fw, fh, cw, ch = fr
        rec["frame"] = {"win": wi, "x": fx, "y": fy, "w": fw, "h": fh,
                        "content_pt": [cw, ch]}
        # Viewport origin in DEVICE px: frame origin + titlebar + tab strip + bar.
        vox, voy = fx, fy + TITLEBAR_PT + VIEW_Y_PT
        rec["viewport_origin_px"] = [vox, voy]
        print("browser frame %r -> viewport origin (%d,%d)" % (fr, vox, voy))

        def to_dev(vx, vy):
            return (vox + vx, voy + vy)

        def aim(x, y):
            """Move and CONFIRM with the guest's own [wm] ptr line."""
            got = ui.settle_pointer(os.path.join(tmp, "ptr.ppm"), x, y)
            return got == (x, y)

        def navigate(page, label):
            mark = len(serial())
            ui.key_mods(["ctrl"], "l")
            time.sleep(0.3)
            ui.typ("http://10.0.2.2:%d/%s" % (port, page))
            ui.key("ret")
            if not wait_for("[browser] load: ", 20.0, mark):
                finish(2, "%s: the address bar never took the URL" % label)
            return mark

        def load_battery(label):
            mark = navigate("battery.html", label)
            if not wait_for("CLK-READY", LOAD_BUDGET, mark):
                finish(2, "%s: battery.html never printed CLK-READY" % label)
            time.sleep(1.0)
            return mark, parse_report(serial()[mark:], "")

        def do_click_dev(x, y, hold=0.12):
            ok = aim(x, y)
            ui.click(hold=hold)
            return ok

        def record(name, action, arg, point, mark, extra=None, wait=EVENT_WAIT,
                   negative=False):
            """Collect every CLK- line the step produced. A positive step waits
            up to `wait` for the first line then a grace period for the rest; a
            negative step waits the whole NEG_WAIT so silence is measured."""
            if negative:
                time.sleep(NEG_WAIT)
            else:
                wait_any("CLK-", wait, mark)
                time.sleep(0.8)
            lines = clk_lines(serial()[mark:])
            ent = {"step": name, "action": action, "arg": arg, "point_view": point,
                   "lines": lines}
            if extra:
                ent.update(extra)
            rec["steps"].append(ent)
            print("  %-18s %-12s %-14s @%-12s -> %s" % (name, action, arg,
                  point, lines if lines else "(nothing)"))
            return ent

        mark0, rep = load_battery("battery")
        rec["report"] = rep
        rec["module_ran"] = "CLK-MODULE-RAN" in serial()[mark0:]
        rec["deferred_ran"] = "CLK-DEFERRED-RAN" in serial()[mark0:]
        print("page view %r scrolly %r module_ran=%s deferred_ran=%s" % (
            rep["view"], rep["scrolly"], rec["module_ran"], rec["deferred_ran"]))
        for k in sorted(rep["rects"]):
            print("   rect %-14s %r hit=%s" % (k, rep["rects"][k], rep["hits"].get(k)))

        # A screendump of the loaded page: where the coloured boxes were PAINTED.
        shot0 = ui.screendump(os.path.join(tmp, "loaded.ppm"))
        p0 = PPM(shot0)
        painted = {}
        for k, rgb in COLOUR.items():
            b = p0.find_color(rgb)
            painted[k] = None if b is None else [b[0] - vox, b[1] - voy, b[2] - vox, b[3] - voy]
        rec["painted_view"] = painted
        print("painted (viewport px, from the screendump): %r" % painted)
        rep2 = None
        painted2 = {}

        for name, action, arg, _doc in PLAN:
            mark = len(serial())
            ui.goto(*PARK, settle=0.1)
            if action in ("click", "click2"):
                src = rep2 if action == "click2" else rep
                if not src or arg not in src["rects"]:
                    record(name, action, arg, None, mark,
                           {"skipped": "no rect for %s in %s" % (arg, "RECT2" if action == "click2" else "RECT")},
                           wait=0.1)
                    continue
                vx, vy = centre(src["rects"][arg])
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok},
                       negative=name in ("blocked", "blocked_clear"))
            elif action == "click_text":
                words = painted_words(serial())
                if arg not in words:
                    record(name, action, arg, None, mark,
                           {"skipped": "word %r not in the [dl] painted-text log" % arg}, wait=0.1)
                    continue
                wx, wy = words[arg]
                # The word's line box is the battery's 40 px row; aim 20 px
                # below its top and ~2 glyphs (14 px) into the run.
                vx, vy = wx + 14, wy - VIEW_Y_PT + 20
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok, "dl_word": [wx, wy]})
            elif action == "click_colour":
                src = painted2 if name.endswith("_paint") and rep2 else painted
                box = src.get(arg)
                if not box:
                    record(name, action, arg, None, mark,
                           {"skipped": "colour %r not found in the screendump" % (COLOUR[arg],)}, wait=0.1)
                    continue
                vx, vy = (box[0] + box[2]) // 2, (box[1] + box[3]) // 2
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok, "painted_box": box})
            elif action == "click_body":
                r = rep["rects"].get(arg)
                if not r:
                    record(name, action, arg, None, mark, {"skipped": "no rect"}, wait=0.1)
                    continue
                vx, vy = r[0] - 40, r[1] + 5       # 40 px left of the probe: bare body
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok})
            elif action == "click_hdr":
                vw = (rep["view"] or (1180, 542))[0]
                vx, vy = vw // 2, 25
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok})
            elif action == "dblclick":
                vx, vy = centre(rep["rects"][arg])
                x, y = to_dev(vx, vy)
                ok = aim(x, y)
                ui.click(hold=0.05)
                ui.click(hold=0.05)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok})
            elif action == "select":
                r = rep["rects"][arg]
                vx, vy = centre(r)
                x, y = to_dev(vx, vy)
                ok = do_click_dev(x, y)
                time.sleep(1.0)
                shot_pop = ui.screendump(os.path.join(tmp, "popup.ppm"))
                # row 1 (0-based) of the popup: browser.c draws rows from
                # (control bottom) + 4, POPUP_ROW px each.
                ry = r[1] + r[3] + 4 + POPUP_ROW * 1 + POPUP_ROW // 2
                x2, y2 = to_dev(vx, ry)
                ok2 = do_click_dev(x2, y2)
                record(name, action, arg, [vx, vy], mark,
                       {"point_dev": [x, y], "row_point_view": [vx, ry],
                        "row_point_dev": [x2, y2], "pointer_confirmed": ok and ok2,
                        "popup_shot": shot_pop})
            elif action == "wheel":
                # Aim inside the viewport (over bare body) so the wheel reaches
                # the page and not the address bar.
                r = rep["rects"].get("body_probe", (1000, 520, 10, 10))
                x, y = to_dev(r[0] - 40, r[1])
                aim(x, y)
                for _ in range(arg):
                    ui._input([{"type": "btn", "data": {"button": "wheel-down", "down": True}},
                               {"type": "btn", "data": {"button": "wheel-down", "down": False}}])
                    time.sleep(0.15)
                got = wait_for("CLK-READY2", 15.0, mark)
                time.sleep(1.0)
                rep2 = parse_report(serial()[mark:], "2") if got else None
                shot2 = ui.screendump(os.path.join(tmp, "scrolled.ppm"))
                p2 = PPM(shot2)
                for k, rgb in COLOUR.items():
                    b = p2.find_color(rgb)
                    painted2[k] = None if b is None else [b[0] - vox, b[1] - voy, b[2] - vox, b[3] - voy]
                rec["report2"] = rep2
                rec["painted2_view"] = painted2
                ent = record(name, action, arg, None, mark,
                             {"ready2": got, "scrolly2": rep2["scrolly"] if rep2 else None,
                              "view2": rep2["view"] if rep2 else None,
                              "rects2": rep2["rects"] if rep2 else None,
                              "painted2": painted2}, wait=0.1)
                print("   after wheel: scrolly2=%r painted2=%r" % (
                    ent["scrolly2"], painted2))
                if rep2:
                    for k in sorted(rep2["rects"]):
                        print("   rect2 %-14s %r hit=%s" % (k, rep2["rects"][k], rep2["hits"].get(k)))

        # ---- the address bar vs the page ------------------------------------
        def browser_died(since):
            return ("[fault] app exception" in serial()[since:] or
                    "[wm] win %d gone" % wi in serial()[since:])

        def relaunch(label):
            """The browser process died: bring a fresh one up and re-derive
            the viewport origin from ITS frame line (a new window need not
            land where the old one was)."""
            nonlocal vox, voy, wi, fx, fy
            m0 = len(serial())
            ui.goto(*PARK, settle=0.2)
            try:
                ui.launch_app("browser")
            except AssertionError as e:
                finish(2, "%s: relaunch after the crash failed: %s" % (label, e))
            time.sleep(7)
            fr2 = None
            for ln in serial()[m0:].splitlines():
                g2 = FRAME_RE.search(ln)
                if g2:
                    fr2 = tuple(int(v) for v in g2.groups())
            if not fr2:
                finish(2, "%s: no frame line after the relaunch" % label)
            wi, fx, fy = fr2[0], fr2[1], fr2[2]
            vox, voy = fx, fy + TITLEBAR_PT + VIEW_Y_PT
            rec.setdefault("relaunches", []).append({"after": label, "frame": fr2,
                                                     "viewport_origin_px": [vox, voy]})
            print("   relaunched the browser after %s: frame %r" % (label, fr2))

        for name, action, arg, _doc in NAV_PLAN:
            if browser_died(mark0):
                relaunch(name)
                mark0 = len(serial())
            if action == "addrbar":
                # A click in the address bar band: VIEW_Y - BARH/2 = 45 pt below
                # the tab strip's top, i.e. frame.y + 30 (titlebar) + 45.
                mark = len(serial())
                x, y = fx + 300, fy + TITLEBAR_PT + 45
                ok = do_click_dev(x, y)
                time.sleep(0.5)
                # The bar has the start page's URL in it; a click puts it in
                # edit mode with the text kept, so clear it the way a person
                # would (Ctrl+A is not an address-bar chord here; backspace).
                for _ in range(80):
                    ui.key("backspace", settle=0.02)
                ui.typ("http://10.0.2.2:%d/%s" % (port, arg))
                ui.key("ret")
                loaded = wait_for("[browser] load: ", 15.0, mark)
                wait_for("CLK-PAGE2", 30.0, mark)
                record(name, action, arg, [300, 45], mark,
                       {"point_dev": [x, y], "pointer_confirmed": ok,
                        "bar_took_click": loaded}, wait=0.1)
                continue
            mark, repn = load_battery(name)
            mark0 = mark
            if arg not in repn["rects"]:
                record(name, action, arg, None, mark, {"skipped": "no rect"}, wait=0.1)
                continue
            vx, vy = centre(repn["rects"][arg])
            x, y = to_dev(vx, vy)
            ui.goto(*PARK, settle=0.1)
            mark = len(serial())
            ok = do_click_dev(x, y)
            if action == "enter":
                time.sleep(0.8)
                ui.key("ret")
            end = time.time() + (8.0 if name == "a_prevent" else 30.0)
            while time.time() < end:
                sl = serial()[mark:]
                if "CLK-PAGE2" in sl or "[fault] app exception" in sl:
                    break
                time.sleep(0.3)
            crashed = browser_died(mark)
            ent = record(name, action, arg, [vx, vy], mark,
                         {"point_dev": [x, y], "pointer_confirmed": ok, "crashed": crashed},
                         wait=0.5)
            if crashed:
                ent["fault"] = [ln for ln in serial()[mark:].splitlines()
                                if "[fault]" in ln or "[core]" in ln]
                print("   BROWSER DIED: %s" % ent["fault"])

        finish(0, "measured")
    finally:
        try:
            proc.kill()
        except OSError:
            pass


# =============================================================================
# THE ORACLE: headless Chrome through playwright, same fixture, same plan
# =============================================================================

def run_oracle(args):
    try:
        from playwright.sync_api import sync_playwright
    except ImportError:
        print("SKIP: playwright is not importable here -- `pip install playwright` "
              "and an installed Google Chrome are what this oracle needs")
        sys.exit(3)
    srv = start_server()
    port = srv.server_port
    base = "http://127.0.0.1:%d/" % port
    vw, vh = (int(v) for v in args.view.split("x"))
    rec = {"mode": "oracle", "view": [vw, vh], "steps": [],
           "started": time.strftime("%Y-%m-%dT%H:%M:%S")}
    lines = []

    with sync_playwright() as p:
        b = p.chromium.launch(channel=args.channel, headless=True)
        rec["browser"] = b.version
        ctx = b.new_context(viewport={"width": vw, "height": vh})
        page = ctx.new_page()
        page.on("console", lambda m: lines.append(m.text))
        page.on("pageerror", lambda e: lines.append("JS exception: %s" % e))

        # PLAYWRIGHT SYNC API TRAP (cost the first run): console events are
        # delivered only while the thread is INSIDE a playwright call, so a
        # time.sleep() here pumps nothing and every wait starves. Every wait
        # below goes through page.wait_for_timeout for that reason.
        def text_since(mark):
            return "\n".join(lines[mark:])

        def wait_any(prefix, secs, mark):
            end = time.time() + secs
            while time.time() < end:
                if any(ln.startswith(prefix) for ln in lines[mark:]):
                    return True
                page.wait_for_timeout(50)
            return False

        def wait_for(needle, secs, mark):
            end = time.time() + secs
            while time.time() < end:
                if needle in text_since(mark):
                    return True
                page.wait_for_timeout(50)
            return False

        def load_battery(label):
            mark = len(lines)
            page.goto(base + "battery.html")
            if not wait_for("CLK-READY", 30, mark):
                raise SystemExit("%s: Chrome never printed CLK-READY" % label)
            page.wait_for_timeout(300)
            return mark, parse_report(text_since(mark), "")

        def record(name, action, arg, point, mark, extra=None, wait=1.5, negative=False):
            if negative:
                page.wait_for_timeout(800)
            else:
                wait_any("CLK-", wait, mark)
                page.wait_for_timeout(300)
            got = clk_lines(text_since(mark))
            ent = {"step": name, "action": action, "arg": arg, "point_view": point, "lines": got}
            if extra:
                ent.update(extra)
            rec["steps"].append(ent)
            print("  %-18s %-12s %-14s @%-12s -> %s" % (name, action, arg, point,
                  got if got else "(nothing)"))
            return ent

        mark0, rep = load_battery("battery")
        rec["report"] = rep
        rec["module_ran"] = "CLK-MODULE-RAN" in text_since(mark0)
        rec["deferred_ran"] = "CLK-DEFERRED-RAN" in text_since(mark0)
        print("chrome %s view %r scrolly %r" % (rec["browser"], rep["view"], rep["scrolly"]))
        for k in sorted(rep["rects"]):
            print("   rect %-14s %r hit=%s" % (k, rep["rects"][k], rep["hits"].get(k)))
        rep2 = None
        for name, action, arg, _doc in PLAN:
            mark = len(lines)
            if action in ("click", "click2"):
                src = rep2 if action == "click2" else rep
                if not src or arg not in src["rects"]:
                    record(name, action, arg, None, mark, {"skipped": "no rect"}, wait=0.1)
                    continue
                vx, vy = centre(src["rects"][arg])
                page.mouse.click(vx, vy)
                record(name, action, arg, [vx, vy], mark,
                       negative=name in ("blocked", "blocked_clear"))
            elif action == "click_text":
                el = TEXT_ELEM[arg]
                vx, vy = centre(rep["rects"][el])
                page.mouse.click(vx, vy)
                record(name, action, arg, [vx, vy], mark, {"note": "chrome: rect of %s" % el})
            elif action == "click_colour":
                # Chrome's rect IS the painted box (its getBoundingClientRect
                # includes transforms), so this row is the rect click again.
                src = rep2 if (name.endswith("_paint") and rep2 and arg in rep2["rects"]) else rep
                vx, vy = centre(src["rects"][arg])
                page.mouse.click(vx, vy)
                record(name, action, arg, [vx, vy], mark, {"note": "chrome: rect == painted"})
            elif action == "click_body":
                r = rep["rects"][arg]
                vx, vy = r[0] - 40, r[1] + 5
                page.mouse.click(vx, vy)
                record(name, action, arg, [vx, vy], mark)
            elif action == "click_hdr":
                vx, vy = vw // 2, 25
                page.mouse.click(vx, vy)
                record(name, action, arg, [vx, vy], mark)
            elif action == "dblclick":
                vx, vy = centre(rep["rects"][arg])
                page.mouse.dblclick(vx, vy)
                record(name, action, arg, [vx, vy], mark)
            elif action == "select":
                # Headless Chrome cannot paint (or be clicked into) the native
                # popup; the click is real, the row pick goes through the
                # select_option path -- stated in the row rather than hidden.
                vx, vy = centre(rep["rects"][arg])
                page.mouse.click(vx, vy)
                page.wait_for_timeout(300)
                page.select_option("#" + arg, "two")
                record(name, action, arg, [vx, vy], mark,
                       {"note": "chrome: row picked via select_option (native popup is not clickable headless)"})
            elif action == "wheel":
                page.mouse.move(vw - 100, vh - 30)
                page.mouse.wheel(0, arg * WHEEL_PX)
                got = wait_for("CLK-READY2", 10, mark)
                page.wait_for_timeout(300)
                rep2 = parse_report(text_since(mark), "2") if got else None
                rec["report2"] = rep2
                record(name, action, arg, None, mark,
                       {"ready2": got, "scrolly2": rep2["scrolly"] if rep2 else None,
                        "rects2": rep2["rects"] if rep2 else None}, wait=0.1)
                if rep2:
                    for k in sorted(rep2["rects"]):
                        print("   rect2 %-14s %r hit=%s" % (k, rep2["rects"][k], rep2["hits"].get(k)))

        for name, action, arg, _doc in NAV_PLAN:
            if action == "addrbar":
                rec["steps"].append({"step": name, "action": action, "arg": arg,
                                     "lines": [], "skipped": "guest-only (Chrome has no LogitOS address bar)"})
                continue
            mark, repn = load_battery(name)
            vx, vy = centre(repn["rects"][arg])
            mark = len(lines)
            page.mouse.click(vx, vy)
            if action == "enter":
                page.wait_for_timeout(200)
                page.keyboard.press("Enter")
            wait_for("CLK-PAGE2", 3 if name == "a_prevent" else 10, mark)
            record(name, action, arg, [vx, vy], mark, wait=0.5)
        b.close()
    srv.shutdown()
    rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
    with open(args.out, "w") as fh:
        json.dump(rec, fh, indent=1)
    print("oracle written to %s" % args.out)


# =============================================================================
# THE VERDICT TABLE
# =============================================================================

def tags_of(lines):
    """The page's own tags only (CLK-xxx minus the geometry report lines)."""
    out = []
    for ln in lines:
        if ln.startswith("CLK-RECT") or ln.startswith("CLK-HIT") or \
           ln.startswith("CLK-VIEW") or ln.startswith("CLK-SCROLLY") or \
           ln.startswith("CLK-READY"):
            continue
        out.append(ln)
    return out


def compare(args):
    with open(args.compare[0]) as fh:
        g = json.load(fh)
    with open(args.compare[1]) as fh:
        o = json.load(fh)
    gs = {s["step"]: s for s in g["steps"]}
    os_ = {s["step"]: s for s in o["steps"]}
    same = diff = 0
    print("%-18s %-7s %-40s | %s" % ("step", "verdict", "guest (LogitOS)", "oracle (Chrome %s)" % o.get("browser")))
    print("-" * 120)
    for name, action, arg, doc in PLAN + NAV_PLAN:
        a = gs.get(name, {}); b = os_.get(name, {})
        ta = tags_of(a.get("lines", [])); tb = tags_of(b.get("lines", []))
        if a.get("skipped") or b.get("skipped"):
            v = "SKIP"
        else:
            # navigation rows: compare the PAGE2 query, not the whole line list
            ka = [t for t in ta if not t.startswith("[browser]")]
            kb = [t for t in tb if not t.startswith("[browser]")]
            v = "SAME" if ka == kb else "DIFF"
            if v == "SAME": same += 1
            else: diff += 1
        print("%-18s %-7s %-40s | %s" % (name, v, " ".join(ta) or "(nothing)", " ".join(tb) or "(nothing)"))
        if a.get("skipped"): print("%-18s         guest skipped: %s" % ("", a["skipped"]))
        if b.get("skipped"): print("%-18s         oracle skipped: %s" % ("", b["skipped"]))
    print("-" * 120)
    print("SAME %d  DIFF %d" % (same, diff))
    # the CSSOM's own hit opinion, side by side
    print("\nelementFromPoint at each rect centre (page-side CSSOM hit test):")
    gh = g.get("report", {}).get("hits", {}); oh = o.get("report", {}).get("hits", {})
    for k in sorted(set(gh) | set(oh)):
        flag = "" if gh.get(k) == oh.get(k) else "   <-- differs"
        print("   %-14s guest=%-14s chrome=%-14s%s" % (k, gh.get(k), oh.get(k), flag))
    print("\nscrollY after the wheel: guest=%r chrome=%r" % (
        (g.get("report2") or {}).get("scrolly"), (o.get("report2") or {}).get("scrolly")))
    print("painted boxes (guest screendump, viewport px) before: %r" % g.get("painted_view"))
    print("painted boxes (guest screendump, viewport px) after wheel: %r" % g.get("painted2_view"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build-atk-click/logit.iso")
    ap.add_argument("--disk", default="build-atk-click/disk.img")
    ap.add_argument("--out", default=None)
    ap.add_argument("--oracle", action="store_true", help="run headless Chrome instead of the guest")
    ap.add_argument("--channel", default="chrome", help="playwright channel (chrome|chromium)")
    ap.add_argument("--view", default="1180x542", help="oracle viewport WxH (use the guest's CLK-VIEW)")
    ap.add_argument("--compare", nargs=2, metavar=("GUEST.json", "ORACLE.json"))
    args = ap.parse_args()
    if args.compare:
        compare(args)
        return
    if not args.out:
        ap.error("--out is required")
    if args.oracle:
        run_oracle(args)
        return
    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            print("HARNESS FAIL: the %s (%s) is missing or empty" % (what, p))
            sys.exit(2)
    run_guest(args)


if __name__ == "__main__":
    main()
