#!/usr/bin/env python3
"""The BLANK-SITE CENSUS driver: one live site, one boot, one FIRST CAUSE.

    python3 tests/qmp/qmp_attack_blank.py --iso build-atk-blank/logit.iso \
        --disk build-atk-blank/disk.img --name bing --url https://www.bing.com/ \
        --out /tmp/atk/bing.json --shots /tmp/atk --qmpdir build-atk-blank/atk-qmp/bing

An INSTRUMENT, like tests/qmp/qmp_site.py, which it imports rather than copies:
the pixel measures, the serial parser, the host probe, the self-test page and
the paced Ctrl+key wrapper are qmp_site's, so a number printed here is the
scoreboard's number and can be compared with it. It exits 0 whether the site
worked or not; the exit code is about the harness.

WHAT IT ADDS TO qmp_site.py, and why each addition is here
==========================================================
The scoreboard verdicts stop at BLANK. This driver's whole purpose is the
sentence AFTER blank: which of five levels killed the page FIRST --

  L1  the document request itself failed or was refused
  L2  a subresource the page cannot live without failed
  L3  the first uncaught exception killed the page's own render
  L4  the DOM exists but CSS/layout makes it invisible
  L5  layout/paint produced boxes but no ink

-- so it collects the evidence for every level on every run, in the precedence
order above, and emits it VERBATIM in the record. The classification itself is
mechanical and is labelled `auto_class`; it is a starting point for the
reader, not a verdict, because "first cause" needs the stack and the box list
read together and no regex does that honestly.

1. `about:boxes` is ALWAYS taken (qmp_site makes it opt-in because the list
   floods the serial console). Here it is the only instrument this tree has
   for level 4: there is NO DOM dump and NO computed-style dump reachable
   from outside the browser (checked 2026-09-02: `about:` recognises exactly
   `text` and `boxes`; `javascript:` URLs are refused by follow_link and not
   executed by load(); DevTools is a drawn panel, F12-toggled, printing
   nothing). The display list is post-layout, so a node that never got a box
   (display:none, or never in the DOM) is absent from it, and one that got a
   box of zero size, or at a negative coordinate, is listed with that size
   and that coordinate. Both are counted below.

2. `about:text` is taken AFTER the dwell and its OWN block is parsed -- not
   the last auto-dump before it, which is what qmp_site reads. The auto-dump
   prints only when the (runs, bytes) pair CHANGES, so on a page that
   re-lays-out with the same words at different coordinates the last
   auto-dump carries the OLD coordinates. about:text prints the record as it
   is now. Both are kept in the record so the difference is visible.

3. THE TEXT DUMP'S COORDINATES ARE SCREEN COORDINATES (browser_paint.c:2711
   passes sx,sy, the post-scroll, post-window-offset position) and THE BOX
   DUMP'S ARE DOCUMENT COORDINATES (e->x, e->y). A negative x in the text
   dump is a run painted to the LEFT OF THE SCREEN; a negative x in the box
   dump is a box laid out left of the document origin. The previous scoreboard
   snapshot's three BLANK rows all carry text at x = -285 .. -388. That is
   the signature this driver was built to count, and it counts it in both
   coordinate spaces.

4. The dwell is a guest-clock quantity where the guest gives one. The
   browser stamps `[wa] t=<ms>` (monotonic_ms) on document-done and on
   network-idle edges, and those stamps are recorded; the dwell itself is
   30 s of host time after `load done`, which under TCG with -rtc clock=host
   is >= 20 s of guest wall time. The record carries the guest stamps so the
   claim "20 s of guest clock" can be checked against the machine's own
   numbers rather than believed; where the guest printed no stamp after
   docdone the record says `guest_dwell_ms: null` rather than a host number.

5. Memory is 1 GiB, the `make run` figure, not the scoreboard's 512 MiB.
   The owner's report is about `make run`. A page that dies at 512 MiB and
   lives at 1 GiB is a real finding but not the one this census is for.
"""

import argparse
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import qmp_site as S                                                # noqa: E402
from qmp_ui import Session, PPM                                     # noqa: E402

BOOT_BUDGET = S.env_f("SITE_BOOT", 300)
LOAD_BUDGET = S.env_f("SITE_LOAD", 240)
SELFTEST_BUDGET = S.env_f("SITE_SELFTEST", 90)
DWELL_HOST = S.env_f("ATK_DWELL", 30)

# The browser's content area in DOCUMENT coordinates at 1280x800, derived from
# qmp_site.VIEWPORT (screen 110..1270 x 200..655): 1160 wide, 455 tall. Used
# only to count boxes "inside the initial viewport"; nothing branches on it.
DOC_VW, DOC_VH = S.VIEWPORT[2] - S.VIEWPORT[0], S.VIEWPORT[3] - S.VIEWPORT[1]

WA_RE = re.compile(r"\[wa\] t=(\d+) (.*)$")
BOX_RE = re.compile(
    r"\[dl\] (\w+)\s+(-?\d+),(-?\d+)\s+(-?\d+)x(-?\d+)\s+<([^>]*)>(.*)$")
TEXTRUN_RE = re.compile(r"^(-?\d+),(-?\d+) (.*)$")


def wa_stamps(text):
    out = []
    for ln in text.splitlines():
        m = WA_RE.search(ln)
        if m:
            out.append((int(m.group(1)), m.group(2).strip()))
    return out


def block_after(text, marker, begin, end):
    """The lines between `begin` and `end` that FOLLOW `marker`. Returns
    (found_marker, lines)."""
    i = text.find(marker)
    if i < 0:
        return False, []
    rest = text[i + len(marker):]
    b = rest.find(begin)
    if b < 0:
        return True, []
    rest = rest[b + len(begin):]
    e = rest.find(end)
    body = rest if e < 0 else rest[:e]
    return True, [ln for ln in body.splitlines() if ln.startswith("[dl] ")]


def parse_text_block(lines):
    runs = []
    for ln in lines:
        m = TEXTRUN_RE.match(ln[5:])
        if m:
            runs.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return runs


def text_stats(runs, screen_w=1280, screen_h=800):
    on = [r for r in runs if 0 <= r[0] < screen_w and 0 <= r[1] < screen_h]
    neg = [r for r in runs if r[0] < 0]
    return {"runs": len(runs), "on_screen": len(on), "neg_x": len(neg),
            "below_screen": len([r for r in runs if r[1] >= screen_h]),
            "right_of_screen": len([r for r in runs if r[0] >= screen_w]),
            "min_x": min([r[0] for r in runs]) if runs else None,
            "max_x": max([r[0] for r in runs]) if runs else None,
            "bytes_on_screen": sum(len(r[2].encode("utf-8")) for r in on),
            "sample_on_screen": [("%d,%d %s" % r) for r in on[:12]],
            "sample_off_screen": [("%d,%d %s" % r) for r in runs if r not in on][:12]}


def parse_boxes(lines):
    items = []
    for ln in lines:
        m = BOX_RE.match(ln)
        if m:
            items.append({"kind": m.group(1), "x": int(m.group(2)), "y": int(m.group(3)),
                          "w": int(m.group(4)), "h": int(m.group(5)),
                          "tag": m.group(6), "rest": m.group(7).strip()})
    return items


def box_stats(items):
    def vis(e):
        return (e["w"] > 0 and e["h"] > 0 and e["x"] + e["w"] > 0 and e["y"] + e["h"] > 0
                and e["x"] < DOC_VW and e["y"] < DOC_VH)
    kinds = {}
    for e in items:
        kinds[e["kind"]] = kinds.get(e["kind"], 0) + 1
    texts = [e for e in items if e["kind"] == "text"]
    return {
        "items": len(items), "kinds": kinds,
        "zero_size": len([e for e in items if e["w"] <= 0 or e["h"] <= 0]),
        "neg_x": len([e for e in items if e["x"] < 0]),
        "neg_y": len([e for e in items if e["y"] < 0]),
        "in_initial_viewport": len([e for e in items if vis(e)]),
        "text_items": len(texts),
        "text_in_initial_viewport": len([e for e in texts if vis(e)]),
        "text_neg_x": len([e for e in texts if e["x"] < 0]),
        "bbox": ([min(e["x"] for e in items), min(e["y"] for e in items),
                  max(e["x"] + e["w"] for e in items), max(e["y"] + e["h"] for e in items)]
                 if items else None),
        "max_y": max([e["y"] + e["h"] for e in items]) if items else None,
        "first": [("%(kind)s %(x)d,%(y)d %(w)dx%(h)d <%(tag)s>%(rest)s" % e)[:140]
                  for e in items[:24]],
        "first_text": [("%(kind)s %(x)d,%(y)d %(w)dx%(h)d <%(tag)s>%(rest)s" % e)[:140]
                       for e in texts[:16]],
    }


def grep_lines(text, pats, cap=40):
    out = []
    for ln in text.splitlines():
        for p in pats:
            if p in ln:
                out.append(ln.strip())
                break
        if len(out) >= cap:
            out.append("... (capped at %d)" % cap)
            break
    return out


DOC_PATS = ["[wa] t=", "docdone", "[browser] page fetch failed", "[tls]", "[bfetch]",
            "[browser] fetch stalled", "[bxfer]", "[browser] load:"]
SUB_PATS = ["script LOST", "script REFUSED", "inserted script", "module fetch FAILED",
            "module rejected", "[browser] cannot fetch", "[browser] fetch failed (status",
            "[css] linked", "[css] fetched", "[css] bytes", "[css] parsed", "[css] dropped",
            "[css] TRUNCATED", "[css] no rule", "[browser] scripts collected",
            "[browser] load done", "[browser] resources:", "[browser] skipping <script",
            "bare module specifier", "[js] module still pending", "[js] module loaded"]
EXC_PATS = ["[browser] JS exception", "[js] uncaught", "[browser] module exception",
            "[js] watchdog", "[js] microtask"]
FATAL_PATS = ["[oom]", "[fault]", "LOGIT_PANIC", "[core]", "heap peak"]


def auto_class(rec):
    """Mechanical first pass over the evidence, precedence L1..L5. Labelled
    `auto` because it is what a regex can see; the report's classification is
    made by reading the verbatim lines this record carries."""
    g = rec["guest"]
    d = rec["doc"]
    if rec["pixels"].get("changed_px", 0) > S.BLANK_MAX and rec["text_after"]["on_screen"] > 0:
        blank = False
    else:
        blank = True
    rec["blank"] = blank
    if d["page_fetch_failed"] or (d["docdone_status"] is not None and d["docdone_status"] >= 400) \
            or (not rec["loaded"] and d["docdone_status"] is None):
        return "L1-document"
    if not blank:
        return "not-blank"
    if rec["sub"]["lost"] or rec["sub"]["refused"] or rec["sub"]["module_failed"] \
            or rec["sub"]["css_transport_err"] or rec["sub"]["css_http_err"]:
        return "L2-subresource(candidate)"
    if g.get("exceptions") or g.get("timer_exceptions") or g.get("module_exceptions"):
        return "L3-exception(candidate)"
    b = rec["boxes"]
    if b["items"] == 0 or b["text_in_initial_viewport"] == 0:
        return "L4-css-invisible"
    return "L5-paint-no-ink"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--shots", required=True)
    ap.add_argument("--qmpdir", required=True,
                    help="a directory of this run's own for qmp.sock and serial.log")
    ap.add_argument("--mem", default="1G")
    args = ap.parse_args()

    os.makedirs(args.shots, exist_ok=True)
    if os.path.isdir(args.qmpdir):
        shutil.rmtree(args.qmpdir)          # a stale qmp.sock makes QEMU refuse to bind
    os.makedirs(args.qmpdir)
    qmp_path = os.path.join(args.qmpdir, "qmp.sock")
    serial_path = os.path.join(args.qmpdir, "serial.log")

    rec = {"name": args.name, "url": args.url, "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "verdict": "HARNESS", "why": "did not run", "mem": args.mem,
           "host": {}, "guest": {}, "pixels": {}, "doc": {}, "sub": {}, "boxes": {},
           "text_after": {}, "text_auto": {}, "loaded": False}
    probe = {}
    rec["host"] = probe
    th = threading.Thread(target=S.host_probe, args=(args.url, probe), daemon=True)
    th.start()

    token = "%d" % (os.getpid() & 0xFFFF)
    page = (S.SELFTEST % token).encode()

    class Selftest(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(page)))
            self.end_headers()
            try:
                self.wfile.write(page)
            except OSError:
                pass

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Selftest)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", args.mem, "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    qlog_path = os.path.join(args.qmpdir, "qemu.log")
    qlog = open(qlog_path, "wb")
    proc = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)

    def serial(frm=0):
        try:
            with open(serial_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace")[frm:]
        except OSError:
            return ""

    def wait_for(needle, secs, frm=0):
        end = time.time() + secs
        while time.time() < end:
            if needle in serial(frm):
                return True
            if proc.poll() is not None:
                return False
            time.sleep(0.4)
        return False

    def finish(verdict, why):
        rec["verdict"] = verdict
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        try:
            dst = os.path.join(args.shots, "%s.serial.full.txt" % args.name)
            with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(serial())
            rec["serial_full"] = dst
        except OSError:
            pass
        th.join(timeout=50)
        probe.pop("_body", None) if False else None
        hb = probe.pop("_body", None)
        if hb:
            hp = os.path.join(args.shots, "%s.host.html" % args.name)
            with open(hp, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(hb)
            rec["host_document"] = hp
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1, ensure_ascii=False)
        print(json.dumps({"name": rec["name"], "verdict": verdict, "why": why,
                          "auto_class": rec.get("auto_class")}, ensure_ascii=False))
        sys.exit(0)

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            finish("HARNESS", "the window manager never brought the desktop up")
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish("HARNESS", str(e))
        time.sleep(7)

        mark = len(serial())
        S.ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("SB-READY-" + token, SELFTEST_BUDGET, mark):
            finish("HARNESS", "the self-test page never loaded -- nothing measured")

        S.ctrl(ui, "t")
        time.sleep(2.0)
        ui.goto(*S.PARK)
        base_ppm = os.path.join(args.qmpdir, "base.ppm")
        ui.screendump(base_ppm, settle=0.8)
        base = PPM(base_ppm)

        typed = None
        for attempt in range(3):
            mark = len(serial())
            S.ctrl(ui, "l")
            ui.typ(args.url)
            t0 = time.time()
            ui.key("ret")
            if not wait_for("[browser] load: ", 8.0, mark):
                continue
            for ln in serial(mark).splitlines():
                i = ln.find("[browser] load: ")
                if i >= 0:
                    typed = ln[i + len("[browser] load: "):].strip()
                    break
            if typed == args.url or typed == args.url.rstrip("/"):
                break
            rec.setdefault("url_mistypes", []).append(typed)
            typed = None
        if typed is None:
            finish("HARNESS", "the URL never reached the address bar intact (%r)"
                   % rec.get("url_mistypes"))
        rec["url_confirmed"] = typed

        loaded = False
        fetch_failed = False
        while time.time() - t0 < LOAD_BUDGET:
            s = serial(mark)
            if "[browser] load done:" in s:
                loaded = True
                break
            if "[browser] page fetch failed" in s:
                fetch_failed = True
                break
            if proc.poll() is not None:
                finish("HARNESS", "QEMU exited during the load")
            time.sleep(0.6)
        rec["loaded"] = loaded
        rec["load_host_s"] = round(time.time() - t0, 1)

        # ---- the dwell: >= DWELL_HOST s of host time after load done/failed ----
        t1 = time.time()
        while time.time() - t1 < DWELL_HOST:
            if proc.poll() is not None:
                finish("HARNESS", "QEMU exited during the dwell")
            time.sleep(2.0)

        ui.goto(*S.PARK)
        after_ppm = os.path.join(args.qmpdir, "after.ppm")
        ui.screendump(after_ppm, settle=0.8)
        after = PPM(after_ppm)
        changed = S.changed_pixels(base, after)
        rec["pixels"] = {
            "changed_px": changed,
            "changed_bbox": S.changed_bbox(base, after),
            "ink_px": after.dark_pixels(S.VIEWPORT, S.INK_THRESH),
            "ink_px_blank": base.dark_pixels(S.VIEWPORT, S.INK_THRESH),
            "colours": S.viewport_colours(after),
            "rich_tiles_proxy": S.rich_tiles(after),
        }
        rec["shot"] = S.ppm_to_png(after_ppm, os.path.join(args.shots, "%s.png" % args.name))
        S.ppm_to_png(base_ppm, os.path.join(args.shots, "%s.blank.png" % args.name))

        def about(word, settle, tries=3):
            for _ in range(tries):
                frm = len(serial())
                S.ctrl(ui, "l")
                ui.typ("about:" + word)
                ui.key("ret")
                if wait_for("[browser] load: about:" + word, 6.0, frm):
                    time.sleep(settle)
                    return True
                time.sleep(1.0)
            return False

        rec["about_text_arrived"] = about("text", 2.5)
        rec["about_boxes_arrived"] = about("boxes", 4.0)
        time.sleep(1.0)

        tail = serial(mark).replace("\r\n", "\n")
        slog = os.path.join(args.shots, "%s.serial.txt" % args.name)
        with open(slog, "w", encoding="utf-8", errors="replace") as fh:
            fh.write(tail)
        rec["serial_log"] = slog

        # ---- the page under test vs the instrument: cut at about:text ----
        cut = tail.find("[browser] load: about:text")
        page_part = tail[:cut] if cut >= 0 else tail
        g = S.parse_serial(page_part)
        rec["guest"] = g

        # guest clock
        st = wa_stamps(page_part)
        rec["wa_stamps"] = ["t=%d %s" % s for s in st][:40]
        tdoc = next((t for t, w in st if w.startswith("docdone")), None)
        rec["guest_docdone_ms"] = tdoc
        rec["guest_last_stamp_ms"] = st[-1][0] if st else None
        rec["guest_dwell_ms"] = (st[-1][0] - tdoc) if (st and tdoc is not None) else None

        # the words: the last auto-dump before about:text, AND about:text's own
        pairs = re.findall(
            r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte[^\n]*\n"
            r"(?:\[dl\] ---8<--- begin painted text\n(.*?)\[dl\] ---8<--- end painted text)?",
            page_part, re.S)
        rec["auto_dumps"] = [(int(a), int(b)) for a, b, _ in pairs]
        if pairs:
            lines = [ln for ln in pairs[-1][2].splitlines() if ln.startswith("[dl] ")]
            rec["text_auto"] = text_stats(parse_text_block(lines))
        else:
            rec["text_auto"] = text_stats([])
        found, tl = block_after(tail, "[browser] load: about:text",
                                "[dl] ---8<--- begin painted text\n",
                                "[dl] ---8<--- end painted text")
        m = re.search(r"\[browser\] load: about:text[^\n]*\n(?:[^\n]*\n)*?"
                      r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte", tail)
        rec["text_after"] = text_stats(parse_text_block(tl))
        rec["text_after"]["dump_found"] = found and bool(tl or (m and int(m.group(1)) == 0))
        rec["text_after"]["reported_runs"] = int(m.group(1)) if m else None
        rec["text_after"]["reported_bytes"] = int(m.group(2)) if m else None

        found_b, bl = block_after(tail, "[browser] load: about:boxes",
                                  "[dl] ---8<--- begin boxes\n", "[dl] ---8<--- end boxes")
        mb = re.search(r"\[dl\] boxes: (\d+) item", tail[tail.find("[browser] load: about:boxes"):]
                       if found_b else "")
        rec["boxes"] = box_stats(parse_boxes(bl))
        rec["boxes"]["dump_found"] = found_b
        rec["boxes"]["reported_items"] = int(mb.group(1)) if mb else None

        # verbatim evidence per level
        docdone = re.search(r"docdone status=(\d+) len=(\d+)", page_part)
        rec["doc"] = {
            "docdone_status": int(docdone.group(1)) if docdone else None,
            "docdone_len": int(docdone.group(2)) if docdone else None,
            "page_fetch_failed": g.get("page_fetch_failed"),
            "lines": grep_lines(page_part, DOC_PATS, 60),
        }
        cssf = re.search(r"\[css\] fetched: (\d+) ok, (\d+) empty, (\d+) http-error, (\d+) transport-error", page_part)
        rec["sub"] = {
            "lost": grep_lines(page_part, ["script LOST", "inserted script LOST"], 30),
            "refused": grep_lines(page_part, ["script REFUSED"], 30),
            "module_failed": grep_lines(page_part, ["module fetch FAILED", "module rejected"], 30),
            "cannot_fetch": g.get("cannot_fetch"),
            "fetch_failed": g.get("fetch_failed"),
            "css_ok": int(cssf.group(1)) if cssf else None,
            "css_empty": int(cssf.group(2)) if cssf else None,
            "css_http_err": int(cssf.group(3)) if cssf else None,
            "css_transport_err": int(cssf.group(4)) if cssf else None,
            "lines": grep_lines(page_part, SUB_PATS, 80),
        }
        rec["exc_lines"] = grep_lines(page_part, EXC_PATS, 40)
        rec["fatal_lines"] = grep_lines(tail, FATAL_PATS, 20)
        rec["auto_class"] = auto_class(rec)

        th.join(timeout=50)
        host_ok = probe.get("ok", False) and 200 <= probe.get("status", 0) < 400
        nexc = len(g["exceptions"]) + len(g["timer_exceptions"]) + len(g["module_exceptions"])
        if g["panic"]:
            finish("CRASH", "the kernel panicked")
        if g["app_fault"]:
            finish("CRASH", "the browser process faulted: " + g["app_fault"])
        if fetch_failed:
            finish("NETWORK" if not host_ok else "FETCH-FAIL",
                   "guest: %s; host status %s" % (g["page_fetch_failed"], probe.get("status")))
        if not loaded:
            finish("NETWORK" if not host_ok else "TIMEOUT",
                   "no `load done` in %.0fs (host status %s)" % (LOAD_BUDGET, probe.get("status")))
        if rec["blank"]:
            finish("BLANK", "changed %d px, %d/%d text runs on screen (%d exceptions)"
                   % (changed, rec["text_after"]["on_screen"], rec["text_after"]["runs"], nexc))
        if nexc:
            finish("ERRORS", "changed %d px, %d text runs on screen, %d exception(s)"
                   % (changed, rec["text_after"]["on_screen"], nexc))
        finish("PAINTED", "changed %d px, %d text runs on screen, no exceptions"
               % (changed, rec["text_after"]["on_screen"]))
    except SystemExit:
        raise
    except Exception as e:                                        # noqa: BLE001
        import traceback
        traceback.print_exc()
        rec["traceback"] = traceback.format_exc()
        finish("HARNESS", "driver error: %r" % (e,))


if __name__ == "__main__":
    main()
