#!/usr/bin/env python3
"""Score ONE real website on the real machine, and emit a machine-readable verdict.

    python3 tests/qmp/qmp_site.py --iso build/logit.iso --disk build/disk.img \
        --name baidu --url https://www.baidu.com/ --out /tmp/baidu.json

This is an INSTRUMENT, not a test. It never fixes anything and it asserts almost
nothing about the browser: it measures, writes down what it saw, and exits 0
whether the site worked or not. The exit code is about the HARNESS (did the
measurement happen), not about the site. `sites_run.py` aggregates the JSON.

WHY IT EXISTS
=============
"I found a bug" is not a number. Fifteen sites can be opened by hand, fourteen
can fail, and the next day nobody can say whether it is now thirteen. The delta
between two dated snapshots is the entire product of this file.

WHAT WAS WRONG WITH THE THROWAWAY THIS REPLACES
===============================================
1. It drove three sites in one boot. Two of the three "failures" were the
   harness: stale window geometry after the first navigation, and residual JS
   state from the previous page. ONE SITE PER BOOT here, no exceptions. The one
   thing that shares the boot is a local self-test page (below), which is served
   by this process, contains no site code, and is bounded by a serial marker so
   nothing it emits can be attributed to the site.

2. It clicked the address bar at a hardcoded (420,145). The browser grew a tab
   strip and that coordinate moved; the click then landed in the tab strip, the
   URL went nowhere, and the resulting screenshot of the previous page was
   reported as a rendering bug that did not exist.

   NO TYPED-IN PIXEL COORDINATES HERE. Navigation is Ctrl+T -- browser.c's own
   "new tab" shortcut, which sets `editing = 1` with an EMPTY url buffer -- then
   the URL, then Enter. That is a keyboard path with no geometry at all, and it
   also removes the backspace-70-times ritual (a URL longer than 70 chars would
   have silently loaded a corrupted address). The one derived coordinate left is
   the Dock icon, and it comes from qmp_ui.dock_icon(), the single copy of that
   arithmetic in the tree.

3. It had no way to tell "the harness misfired" from "the site failed". Here,
   before the site is touched, the guest is made to load a page THIS PROCESS
   serves, whose only content is `console.log('SB-READY-<token>')`. That one
   marker proves, in one step: the keyboard reaches the app, Ctrl+T focuses the
   address bar, typing fills it, Enter loads, the network works, HTML parses and
   JS runs. If it does not appear, the verdict is HARNESS and the site is not
   scored at all -- because at that point we have measured nothing about it.

MEASUREMENT, AND WHY EACH ONE IS THE ONE CHOSEN
===============================================
* `changed_px` -- pixels that differ from a screenshot of the SAME BOOT taken
  with an empty tab open, seconds earlier. This is the primary "did anything
  render" number and it is deliberately not an ink threshold: `ink` counts dark
  pixels, and a dark-themed page (bing renders "一坨黑黑的") has a near-black
  background, so its blank state and its rendered state are both "full of ink".
  A same-boot difference has no such failure mode and needs no window geometry.
* `ink_px` / `colours` -- kept as secondaries and comparable with the existing
  qmp_browser_https.py numbers, over the same viewport rectangle. Reported, not
  judged.
* `rich_tiles` -- 16x16 tiles holding more than 24 distinct colours. A proxy for
  photographic content, because the browser prints no image-load count (see
  FINDINGS in the report). Labelled a proxy on purpose; do not read it as an
  image count.
* Every distinct JS exception WITH ITS STACK. js_page.c prints the stack after
  the message since 84b6aef, so the frames are on the serial and are captured
  here; a message alone ("cannot read property 'charAt' of undefined") names the
  operation and nothing else.

WHAT THE VERDICTS MEAN, AND WHAT THE TOP ONE DOES NOT
=====================================================
CRASH, HARNESS, TIMEOUT, FETCH-FAIL, BLANK, ERRORS, GAP, FLAKY, NETWORK,
PAINTED -- worst first, and that is the order the table prints in.

The top verdict is PAINTED and it used to be called OK. OK was wrong, and the
user said so about their own machine: deepseek painted, and painted WRONG, and
was published as OK for it. PAINTED means exactly three measured things -- more
than BLANK_MAX pixels changed against an empty tab, no script threw, and the
guest requested everything the document requires. It does NOT mean the right
pixels changed. Nothing on this machine checks that; reftests do, and none of
WPT's run here.

GAP is the verdict for a page that painted and threw nothing and still never
asked for part of itself. See the comment above inventory() for why that class
was invisible until it had its own verdict.

HONEST ABOUT THE NETWORK
========================
These are live sites over QEMU SLIRP. They rate-limit, they A/B, they go down.
So the same URL is fetched FROM THE HOST, in parallel with the boot, with the
same User-Agent the browser sends: status, redirect chain, byte count, and the
document's own <script>/<img> counts. If the host cannot reach the site either,
the verdict is NETWORK and the run is not scored against the browser. If the
host got a 200 and the guest did not, that difference is the finding.
"""

import argparse
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
import zlib
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM, dock_icon, BROWSER_SLOT          # noqa: E402

# The User-Agent the guest's own fetcher sends, so the host probe is offered the
# same document. A site that serves a different page to an unknown UA would
# otherwise make the host probe's script/img counts a lie about what the guest
# was given.
# It is browser_rt.c's exact string. Sites sniff it: baidu serves OUR UA a
# 227-byte redirect stub and a real browser a real page, so a host probe run
# under Python-urllib's default would be comparing two different documents and
# calling the difference a browser bug.
UA = os.environ.get("SITE_UA", "Mozilla/5.0 (X11; LogitOS x86_64) Logit/1.0")

# The viewport rectangle from qmp_browser_https.py, reused verbatim so the two
# harnesses' ink numbers are comparable. Nothing here BRANCHES on it.
VIEWPORT = (110, 200, 1270, 655)
INK_THRESH = 160
PARK = (55, 400)                       # no window, no dock: keep the pointer out of the count

# A page that renders is tens of thousands of changed pixels. An empty viewport
# with only the tab title and the status line redrawn is a few hundred. 4000 is
# an order of magnitude clear of both, and the raw number is always reported so
# a borderline case is visible rather than rounded into a verdict.
BLANK_MAX = 4000


def env_f(name, dflt):
    try:
        return float(os.environ.get(name, dflt))
    except ValueError:
        return float(dflt)


BOOT_BUDGET = env_f("SITE_BOOT", 300)
LOAD_BUDGET = env_f("SITE_LOAD", 240)
PAINT_BUDGET = env_f("SITE_PAINT", 75)
SELFTEST_BUDGET = env_f("SITE_SELFTEST", 90)


# ---------------------------------------------------------------- PNG output
# A screendump is a 3 MB PPM. Eighteen sites x two shots is 110 MB of artefact
# nobody can open in a browser, so they are re-encoded here. Hand-rolled because
# the rest of tests/qmp/ is dependency-free and this is thirty lines.

def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                                   # filter: none
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body) & 0xFFFFFFFF))

    hdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n")
        fh.write(chunk(b"IHDR", hdr))
        fh.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        fh.write(chunk(b"IEND", b""))


def ppm_to_png(ppm_path, png_path):
    p = PPM(ppm_path)
    write_png(png_path, p.w, p.h, p.px)
    return png_path


# ------------------------------------------------------------ pixel measures

def changed_pixels(a, b, thresh=24, step=2):
    """Pixels differing between two screendumps, sampled every `step` in x and y
    and scaled back up. Full precision costs ~1.5 s per comparison and this is
    called in a settle loop; the sample is documented rather than hidden, and
    the number it produces is an estimate of a quantity whose only use is
    "thousands vs hundreds"."""
    if a.w != b.w or a.h != b.h:
        return -1
    pa, pb = a.px, b.px
    row = a.w * 3
    n = 0
    for y in range(0, a.h, step):
        base = y * row
        for x in range(0, a.w, step):
            o = base + x * 3
            if (abs(pa[o] - pb[o]) > thresh or abs(pa[o + 1] - pb[o + 1]) > thresh
                    or abs(pa[o + 2] - pb[o + 2]) > thresh):
                n += 1
    return n * step * step


def changed_bbox(a, b, thresh=24, step=4):
    if a.w != b.w or a.h != b.h:
        return None
    pa, pb = a.px, b.px
    row = a.w * 3
    x0 = y0 = 1 << 30
    x1 = y1 = -1
    for y in range(0, a.h, step):
        base = y * row
        for x in range(0, a.w, step):
            o = base + x * 3
            if (abs(pa[o] - pb[o]) > thresh or abs(pa[o + 1] - pb[o + 1]) > thresh
                    or abs(pa[o + 2] - pb[o + 2]) > thresh):
                if x < x0: x0 = x
                if x > x1: x1 = x
                if y < y0: y0 = y
                if y > y1: y1 = y
    return None if x1 < 0 else [x0, y0, x1, y1]


def viewport_colours(p, step=3):
    seen = set()
    x0, y0, x1, y1 = VIEWPORT
    for y in range(y0, min(p.h, y1), step):
        for x in range(x0, min(p.w, x1), step):
            seen.add(p.at(x, y))
    return len(seen)


def rich_tiles(p, tile=16, minc=24):
    """Tiles inside the viewport holding many distinct colours -- a PROXY for
    photographic content, not an image count. The browser emits no image-load
    count on the serial console (see the report's FINDINGS), so this is the only
    signal available without editing browser.c, which this line may not do."""
    x0, y0, x1, y1 = VIEWPORT
    n = 0
    for ty in range(y0, min(p.h, y1) - tile, tile):
        for tx in range(x0, min(p.w, x1) - tile, tile):
            seen = set()
            for y in range(ty, ty + tile, 2):
                for x in range(tx, tx + tile, 2):
                    seen.add(p.at(x, y))
                    if len(seen) > minc:
                        break
                if len(seen) > minc:
                    break
            if len(seen) > minc:
                n += 1
    return n


# ------------------------------------------------------------- serial parsing

EXC_RE = re.compile(r"\[browser\] JS exception: (.*)$")
FRAME_RE = re.compile(r"^\s+at .*$")
LOADDONE_RE = re.compile(
    r"\[browser\] load done: (\d+) requests, (\d+) connections dialled, (\d+) reused"
    r", (\d+) modules loaded \((\d+) failed\)")


def parse_serial(text):
    """Everything the guest said about this navigation."""
    out = {
        "exceptions": [],          # [{message, stack:[frames], count}]
        "timer_exceptions": [],
        "module_exceptions": [],
        "fetch_failed": [],
        "cannot_fetch": [],
        "skipped_scripts": 0,
        "requests": None, "dials": None, "reused": None,
        "modules": None, "modules_failed": None,
        "heap_peak_k": None,
        "load_done": False,
        "page_fetch_failed": None,
        "app_fault": None,
        "panic": False,
    }
    lines = text.splitlines()
    i = 0
    seen = {}
    while i < len(lines):
        ln = lines[i]
        m = EXC_RE.search(ln)
        if m:
            msg = m.group(1).strip()
            frames = []
            j = i + 1
            # QuickJS's `stack` string starts with the error's own message line,
            # unindented, and js_page.c prints it verbatim after its own message
            # line. Skipping that repeat is what makes the frames reachable --
            # without it the frame scan stops on the very first line and every
            # exception in the scoreboard came out with an empty stack.
            if j < len(lines) and lines[j].strip() == msg:
                j += 1
            while j < len(lines) and FRAME_RE.match(lines[j]):
                frames.append(lines[j].strip())
                j += 1
            # DISTINCT means distinct message AND distinct top frame. A bundle
            # that throws the same `cannot read property 'x' of undefined` from
            # two different modules is two findings, and merging them on the
            # message would hide one of them completely.
            key = (msg, frames[0] if frames else "")
            if key in seen:
                seen[key]["count"] += 1
            else:
                seen[key] = {"message": msg, "stack": frames, "count": 1}
                out["exceptions"].append(seen[key])
            i = j
            continue
        if "[js] uncaught in " in ln:
            out["timer_exceptions"].append(ln.split("[js] uncaught in ", 1)[1].strip())
        elif "[browser] module exception in " in ln:
            out["module_exceptions"].append(
                ln.split("[browser] module exception in ", 1)[1].strip())
        elif "[browser] module rejected " in ln:
            out["module_exceptions"].append(
                ln.split("[browser] module rejected ", 1)[1].strip())
        elif "[browser] fetch failed (status " in ln:
            out["fetch_failed"].append(ln.split("[browser] ", 1)[1].strip())
        elif "[browser] cannot fetch " in ln:
            out["cannot_fetch"].append(ln.split("[browser] cannot fetch ", 1)[1].strip())
        elif '[browser] skipping <script' in ln:
            out["skipped_scripts"] += 1
        elif "[browser] page fetch failed: " in ln:
            out["page_fetch_failed"] = ln.split("page fetch failed: ", 1)[1].strip()
        elif "[browser] heap peak " in ln:
            try:
                out["heap_peak_k"] = int(ln.split("heap peak ", 1)[1].rstrip("K \r"))
            except ValueError:
                pass
        elif "[fault] app exception" in ln:
            out["app_fault"] = ln.strip()
        elif "LOGIT_PANIC" in ln:
            out["panic"] = True
        d = LOADDONE_RE.search(ln)
        if d:
            out["load_done"] = True
            (out["requests"], out["dials"], out["reused"],
             out["modules"], out["modules_failed"]) = [int(g) for g in d.groups()]
        i += 1
    return out


# ------------------------------------- what the document asked a browser to get
#
# THE HOLE THIS CLOSES, stated plainly because the instrument shipped with it.
#
# The first baseline scored stripe.com as its top verdict. Its record said 80
# requests, zero failed fetches, 585 colours. The user then opened the same page
# themselves and counted SEVENTY-TWO STYLESHEETS, none of them applied. Both
# observations were true: the guest issued 80 requests against 74 script srcs
# plus the document, and there is no room in that number for 72 stylesheets. The
# browser never asked for them. `fetch_failed` was empty because A REQUEST THAT
# IS NEVER MADE CANNOT FAIL, and an instrument built out of failure counters is
# blind to that whole class by construction -- silently, and in favour of the
# browser.
#
# So the document is inventoried here and compared with the number of requests
# the guest actually issued. The comparison is deliberately ONE-SIDED: it fires
# only when the guest issued FEWER requests than the document's mandatory
# synchronous set, which no amount of deduplication, caching or capping can
# explain away. A page that requests more than the inventory (its own dynamic
# loader, images, redirects) produces no gap, and a gap that is real but smaller
# than the slack is missed. False negatives, never false positives -- the same
# trade the rest of this file makes.

_ATTR = r'\b%s\s*=\s*(?:"([^"]*)"|\'([^\']*)\'|([^\s>]+))'


def _attr(tag, name):
    m = re.search(_ATTR % name, tag, re.I)
    if not m:
        return None
    return m.group(1) or m.group(2) or m.group(3) or ""


# js_module.c's JavaScript-MIME whitelist. Anything else is a data block that a
# browser must NOT execute or fetch as script (importmap, application/json,
# text/template), so counting it would invent a gap that is not there.
JS_TYPES = ("text/javascript", "application/javascript", "application/x-javascript",
            "text/ecmascript", "application/ecmascript", "text/jscript",
            "text/javascript1.5", "text/x-javascript", "javascript")
# browser.c collect_css_links() drops these on purpose: they are inactive
# accessibility override themes, and skipping them is a correctness filter, not
# a budget. Mirrored here so they are not counted as missing.
CSS_SKIP = ("high_contrast", "colorblind", "tritanopia")


def inventory(body):
    """The subresources the document asks for, filtered the way browser.c filters
    them, deduplicated the way browser.c deduplicates them (by the raw attribute
    value). Anything this over-counts becomes a false gap, so every filter here
    exists to match a filter in the loader."""
    sheets, scripts, preloads, fonts, imgs = set(), set(), set(), set(), set()
    inline_scripts = 0

    for tag in re.findall(r"<link\b[^>]*>", body, re.I):
        href = _attr(tag, "href") or ""
        rel = (_attr(tag, "rel") or "").lower()
        if not href or href.lower().startswith("data:"):
            continue
        low = href.lower()
        if any(s in low for s in CSS_SKIP):
            continue
        if "stylesheet" in rel:
            sheets.add(href)
        elif "preload" in rel or "prefetch" in rel or "modulepreload" in rel:
            (fonts if (_attr(tag, "as") or "").lower() == "font" else preloads).add(href)

    for m in re.finditer(r"<script\b([^>]*)>", body, re.I):
        tag = "<script" + m.group(1) + ">"
        typ = (_attr(tag, "type") or "").strip().lower().split(";")[0]
        module = typ == "module"
        if typ and not module and typ not in JS_TYPES:
            continue                                     # a data block, not code
        if not module and re.search(r"\bnomodule\b", m.group(1), re.I):
            continue                                     # a fallback we do not need
        src = _attr(tag, "src")
        if src:
            if src.lower().startswith(("data:", "javascript:")):
                continue
            scripts.add(src)
        else:
            inline_scripts += 1

    for tag in re.findall(r"<img\b[^>]*>", body, re.I):
        src = _attr(tag, "src") or ""
        if src and not src.lower().startswith("data:"):
            imgs.add(src)

    inv = {"stylesheets": len(sheets), "script_src": len(scripts),
           "inline_scripts": inline_scripts, "images": len(imgs),
           "preloads": len(preloads), "fonts": len(fonts)}
    # THE MANDATORY SET: the document itself, every external classic/module
    # script, and every stylesheet. browser.c fetches all three unconditionally
    # and before it paints. Images are excluded because the loader caps them
    # (res_add stops at 16), and preloads because they are a hint a browser is
    # allowed to ignore -- counting either would produce a gap the loader is
    # entitled to.
    inv["mandatory"] = 1 + len(scripts) + len(sheets)
    return inv


# ------------------------------------------------------------- the host probe

def host_probe(url, result):
    """Fetch the same URL from the host. Answers 'is the site up and what did it
    serve', which is the only way a live-web run can tell our failure from
    theirs."""
    import urllib.request
    import urllib.error
    import gzip
    import io

    chain = []

    class Redirects(urllib.request.HTTPRedirectHandler):
        def redirect_request(self, req, fp, code, msg, headers, newurl):
            chain.append("%d -> %s" % (code, newurl))
            return super().redirect_request(req, fp, code, msg, headers, newurl)

    t0 = time.time()
    try:
        op = urllib.request.build_opener(Redirects)
        hdrs = {"Accept": "text/html,application/xhtml+xml,*/*",
                "Accept-Encoding": "gzip, identity"}
        if UA:
            hdrs["User-Agent"] = UA
        req = urllib.request.Request(url, headers=hdrs)
        with op.open(req, timeout=45) as r:
            raw = r.read(4 << 20)
            if r.headers.get("Content-Encoding", "") == "gzip":
                try:
                    raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
                except OSError:
                    pass
            body = raw.decode("utf-8", "replace")
            result.update(
                ok=True, status=r.status, final_url=r.geturl(),
                bytes=len(raw), redirects=chain,
                script_tags=len(re.findall(r"<script\b", body, re.I)),
                script_src=len(re.findall(r"<script\b[^>]*\bsrc=", body, re.I)),
                img_tags=len(re.findall(r"<img\b", body, re.I)),
                inventory=inventory(body),
                elapsed=round(time.time() - t0, 1))
            # KEEP THE DOCUMENT. Everything above is a COUNT taken from it, and
            # a count cannot be re-questioned: the moment anyone asks "which
            # script defines that symbol" the bytes are gone, and on a live
            # site they cannot be fetched again -- bing serves a different
            # document per request, so a curl an hour later is a different
            # page and any conclusion drawn from it is unfalsifiable. Measured
            # the day this was added: a `_w is not defined` investigation ran
            # aground on exactly that.
            #
            # Under `_body` and popped by the caller rather than stored in the
            # JSON: it is 217 KB for bing and over a megabyte for github, and
            # the record is meant to be read.
            result["_body"] = body
    except urllib.error.HTTPError as e:
        result.update(ok=True, status=e.code, final_url=url, bytes=0,
                      redirects=chain, script_tags=0, script_src=0, img_tags=0,
                      inventory=None, elapsed=round(time.time() - t0, 1))
    except Exception as e:                                        # noqa: BLE001
        result.update(ok=False, error="%s: %s" % (type(e).__name__, e),
                      elapsed=round(time.time() - t0, 1))


# ------------------------------------------------------------------ the guest

def ctrl(ui, qcode):
    """Ctrl+<key>, PACED.

    THIS NEVER WORKED, and it took a second navigation to find out. It used to
    put ctrl-down and key-down in ONE input-send-event and the two releases in
    another -- four scancodes in two bursts, into a PS/2 controller with a
    ONE-BYTE buffer. The letter was dropped every time.

    Nothing noticed for a year because of a coincidence: browser.c starts with
    `editing = 1`, so the address bar is already focused when this harness
    types its first URL, and Ctrl+L not arriving is indistinguishable from
    Ctrl+L working. The SECOND navigation in a boot is where it shows, and
    until about:text there was never a second one.

    qmp_ui.Session.key_mods() is the paced version; this wrapper stays because
    a dozen call sites read better as ctrl(ui, "l")."""
    ui.key_mods(("ctrl",), qcode, settle=0.25)


SELFTEST = ("<!doctype html><html><head><title>sb</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('SB-READY-%s');</script></body></html>")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--name", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True, help="where to write the JSON record")
    ap.add_argument("--shots", default=None, help="directory for the screenshots")
    ap.add_argument("--keep", action="store_true", help="keep the serial log and PPMs")
    args = ap.parse_args()

    shots_dir = args.shots or os.path.dirname(os.path.abspath(args.out))
    os.makedirs(shots_dir, exist_ok=True)

    rec = {
        "name": args.name, "url": args.url,
        "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "verdict": "HARNESS", "why": "did not run",
        "host": {}, "guest": {}, "pixels": {}, "shot": None, "serial_log": None,
    }

    probe = {}
    rec["host"] = probe
    th = threading.Thread(target=host_probe, args=(args.url, probe), daemon=True)
    th.start()

    tmp = tempfile.mkdtemp(prefix="site_%s_" % re.sub(r"\W+", "_", args.name))
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")
    rec["serial_log"] = serial_path

    token = "%d" % (os.getpid() & 0xFFFF)
    page = (SELFTEST % token).encode()

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
           # -snapshot ALWAYS. The browser persists its session (tabs, history)
           # to /browser/* on the LogitFS disk, so without it the second site
           # scored would boot with the first site's tab restored -- exactly the
           # shared-state class of false failure this file exists to avoid.
           "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]
    # Debug hook: SITE_QEMU_EXTRA="-s" (etc.) appends raw QEMU args -- the
    # qwen wedge was chased by attaching gdb to the stub mid-hang, which needs
    # exactly this and nothing else changed about the boot.
    extra = os.environ.get("SITE_QEMU_EXTRA", "")
    if extra:
        import shlex
        cmd += shlex.split(extra)
    if os.environ.get("SITE_PCAP"):
        cmd[-2:-2] = ["-object", "filter-dump,id=d0,netdev=n0,file=%s"
                      % os.path.join(tmp, "net.pcap")]

    # QEMU's OWN OUTPUT, KEPT. It was discarded once, and the whole corpus then
    # came back "the kernel never printed LOGIT_BOOT_OK" -- 36 boots, one
    # message, and the truth was that a concurrent `make` in this shared worktree
    # had deleted build/disk.img, so QEMU exited before executing an instruction.
    # A harness that turns "the file is missing" into "the kernel is broken" is
    # the exact failure this line exists to stop reporting.
    qlog_path = os.path.join(tmp, "qemu.log")
    qlog = open(qlog_path, "wb")
    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["verdict"] = "HARNESS"
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            with open(args.out, "w", encoding="utf-8") as fh:
                json.dump(rec, fh, indent=1, ensure_ascii=False)
            print(json.dumps({"name": args.name, "verdict": "HARNESS",
                              "why": rec["why"]}))
            sys.exit(0)
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

    def qemu_tail():
        try:
            qlog.flush()
        except (OSError, ValueError):
            pass
        try:
            with open(qlog_path, "rb") as fh:
                return fh.read().decode("utf-8", "replace").strip()[-400:]
        except OSError:
            return ""

    def finish(verdict, why):
        if verdict == "HARNESS" and proc.poll() is not None:
            t = qemu_tail()
            why += " [QEMU exited %s%s]" % (proc.returncode,
                                            (": " + t) if t else "")
        rec["verdict"] = verdict
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        if rec.get("serial_log") == serial_path:      # an early exit: keep the log anyway
            try:
                dst = os.path.join(shots_dir, "%s.serial.txt" % args.name)
                with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                    fh.write(serial())
                rec["serial_log"] = dst
            except OSError:
                pass
        th.join(timeout=50)
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(rec, fh, indent=1, ensure_ascii=False)
        print(json.dumps({"name": rec["name"], "verdict": verdict, "why": why},
                         ensure_ascii=False))
        sys.exit(0)

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            finish("HARNESS", "the window manager never brought the desktop up")
        time.sleep(3)

        ui = Session(qmp_path, serial=serial_path)
        ui.click_at(*dock_icon(BROWSER_SLOT))
        for _ in range(5):
            if wait_for("launched Browser", 15):
                break
            ui.click_at(*dock_icon(BROWSER_SLOT))
        else:
            finish("HARNESS", "the Dock never launched the Browser")
        time.sleep(7)                  # ~3 MB .aex off virtio-blk, ELF load, first paint

        # ---- the self-test: prove the whole navigation path before scoring ----
        mark = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("SB-READY-" + token, SELFTEST_BUDGET, mark):
            finish("HARNESS",
                   "the self-test page never loaded -- the keyboard, Ctrl+T, the "
                   "address bar or SLIRP is broken, so nothing was measured about "
                   "this site")

        # ---- a fresh tab, and the control screenshot for this boot ----
        # The tab is new so its URL buffer is empty and `editing` is already 1;
        # Ctrl+L is pressed again after the screenshot purely because parking the
        # pointer moves the mouse across the window, and a stray mouse event
        # reaching the page would take focus off the bar. Ctrl+L is idempotent
        # here -- it sets editing and touches nothing else -- so re-asserting it
        # costs nothing and removes the only way this step can silently misfire.
        ctrl(ui, "t")
        time.sleep(2.0)
        ui.goto(*PARK)
        base_ppm = os.path.join(tmp, "base.ppm")
        ui.screendump(base_ppm, settle=0.8)
        base = PPM(base_ppm)

        # ---- the site ----
        mark = len(serial())
        ctrl(ui, "l")
        ui.typ(args.url)
        t0 = time.time()
        ui.key("ret")

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
        load_s = round(time.time() - t0, 1)

        # ---- let it paint and let its async work run, then photograph ----
        after_ppm = os.path.join(tmp, "after.ppm")
        t1 = time.time()
        last = -1
        changed = 0
        settled = False
        # A MINIMUM DWELL, not just a stability test. `load done` is printed when
        # the classic scripts have run; fetches, promise reactions and timers keep
        # going after it, and so do the exceptions they throw. Two equal samples
        # inside the first few seconds would mean "nothing has happened yet", not
        # "it has finished", so stability only counts from 15 s on.
        # Stability is a TOLERANCE, not equality. The menu bar carries a running
        # clock and the status line counts sheets, so two consecutive frames of a
        # completely finished page are never byte-identical -- an equality test
        # here simply never fires and every site pays the whole paint budget.
        MIN_DWELL = 15.0
        while time.time() - t1 < PAINT_BUDGET:
            ui.goto(*PARK)
            ui.screendump(after_ppm, settle=0.6)
            after = PPM(after_ppm)
            changed = changed_pixels(base, after)
            if (last >= 0 and abs(changed - last) <= max(400, changed // 50)
                    and time.time() - t1 >= MIN_DWELL):
                settled = True
                break
            last = changed
            time.sleep(3)
        after = PPM(after_ppm)
        paint_s = round(time.time() - t1, 1)

        png = os.path.join(shots_dir, "%s.png" % args.name)
        ppm_to_png(after_ppm, png)
        base_png = os.path.join(shots_dir, "%s.blank.png" % args.name)
        ppm_to_png(base_ppm, base_png)
        rec["shot"] = png

        # WHICH WORDS REACHED THE SCREEN. `changed px` above cannot tell a
        # rendered page from a flat dark block -- this file's own header says
        # so -- and the gap is not academic: bilibili scores PAINTED with
        # 267,376 changed pixels and every one of its video cards is a
        # thumbnail above an EMPTY grey rectangle where the title should be.
        # No exception, no failed request, nothing in the record.
        #
        # Ctrl+Alt+D makes the browser print the text runs its last paint
        # emitted (c/apps/browser/browser_paint.h). Sent AFTER the screenshot,
        # so the dump cannot disturb the pixels that were just measured, and
        # after the page has settled, so what it reports is the finished page.
        # THE ADDRESS BAR, not a chord. Ctrl+Alt+D is wired in the browser and
        # is the convenient trigger for a person, but it produced no output
        # here across two runs -- unpaced and paced one scancode at a time --
        # and nothing in the kernel explains why (kbd_mods reports EV_MOD_ALT;
        # wm_shortcut only claims SUPER). An instrument whose trigger cannot be
        # observed is not an instrument, so this uses the channel every driver
        # in this directory already exercises forty times a run.
        #
        # about:text does NOT navigate -- it prints and returns, leaving the
        # page and its last paint exactly where they were, which is the whole
        # point: the question is about the page that is loaded.
        ctrl(ui, "l")
        ui.typ("about:text")
        ui.key("ret")
        time.sleep(2.5)

        # The document the HOST was served, beside the log for the same
        # reason. Not the guest's copy -- we have no way to read that back --
        # but fetched with the guest's own User-Agent, in parallel with the
        # boot, so it is the closest thing to the bytes the browser saw. A
        # site that serves two different documents to one UA in one minute
        # will still defeat it, and the file makes that visible instead of
        # leaving it to be assumed.
        hb = (rec.get("host") or {}).pop("_body", None)
        if hb:
            hp = os.path.join(shots_dir, "%s.host.html" % args.name)
            try:
                with open(hp, "w", encoding="utf-8", errors="replace") as fh:
                    fh.write(hb)
                rec["host_document"] = hp
            except OSError:
                pass

        # The serial from the moment Enter was pressed, kept BESIDE the JSON: a
        # verdict without the log that produced it cannot be argued with.
        tail = serial(mark)
        # THE LAST summary, not the first. browser_paint prints one line every
        # time the pair CHANGES, so the first is an early frame -- often the
        # empty tab -- and reading it would report a settled page's text as
        # whatever was on screen a second after Enter.
        # CRLF FIRST. The serial log is CRLF and every regex below anchors on
        # a newline; run-net-bench.sh's own note records the same trap in a
        # BRE. Without this the summary matched (it does not span lines) and
        # the block never did, so the record carried a count and no words --
        # which reads as "the page painted nothing recognisable" rather than
        # "the harness could not read its own instrument".
        tail_n = tail.replace("\r\n", "\n")
        ms = re.findall(r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte", tail_n)
        m = ms[-1] if ms else None
        if m:
            rec["text_runs"] = int(m[0])
            rec["text_bytes"] = int(m[1])
            body = re.search(r"\[dl\] ---8<--- begin painted text\n(.*?)"
                             r"\[dl\] ---8<--- end painted text", tail_n, re.S)
            if body:
                lines = [ln[5:] for ln in body.group(1).splitlines()
                         if ln.startswith("[dl] ")]
                rec["text"] = "\n".join(lines)
        else:
            # Absent is a finding, not a blank: a page that painted no text at
            # all and a dump that did not happen are different, and only the
            # second one is the harness's fault.
            rec["text_runs"] = None
        slog = os.path.join(shots_dir, "%s.serial.txt" % args.name)
        with open(slog, "w", encoding="utf-8", errors="replace") as fh:
            fh.write(tail)
        rec["serial_log"] = slog

        g = parse_serial(tail)
        g["load_seconds"] = load_s
        g["paint_seconds"] = paint_s
        g["settled"] = settled
        rec["guest"] = g
        rec["pixels"] = {
            "changed_px": changed,
            "changed_bbox": changed_bbox(base, after),
            "ink_px": after.dark_pixels(VIEWPORT, INK_THRESH),
            "ink_px_blank": base.dark_pixels(VIEWPORT, INK_THRESH),
            "colours": viewport_colours(after),
            "rich_tiles_proxy": rich_tiles(after),
        }

        th.join(timeout=50)
        host_ok = probe.get("ok", False) and 200 <= probe.get("status", 0) < 400

        nexc = (len(g["exceptions"]) + len(g["timer_exceptions"])
                + len(g["module_exceptions"]))

        # THE SUBRESOURCE GAP. See the comment above inventory(): one-sided, so
        # a positive number is a claim that the guest issued fewer requests than
        # the document's mandatory set, which nothing legitimate explains.
        inv = probe.get("inventory")
        gap = None
        if inv and g.get("requests") is not None and loaded:
            short = inv["mandatory"] - g["requests"]
            if short > 0:
                gap = {"mandatory": inv["mandatory"], "requested": g["requests"],
                       "short_by": short, "stylesheets": inv["stylesheets"],
                       "script_src": inv["script_src"]}
        rec["subresources"] = {"host_inventory": inv, "gap": gap}

        # ---- the verdict ----
        if g["panic"]:
            finish("CRASH", "the kernel panicked")
        if g["app_fault"]:
            finish("CRASH", "the browser process faulted: " + g["app_fault"])
        if fetch_failed:
            if not host_ok:
                finish("NETWORK", "neither the guest nor the host could fetch it (%s)"
                       % probe.get("error", "host status %s" % probe.get("status")))
            finish("FETCH-FAIL", "the guest could not fetch it (%s) while the host "
                                 "got HTTP %s" % (g["page_fetch_failed"], probe.get("status")))
        if not loaded:
            if not host_ok:
                finish("NETWORK", "no load in %.0fs and the host could not fetch it "
                                  "either (%s)" % (LOAD_BUDGET, probe.get("error", "?")))
            finish("TIMEOUT", "no `load done` in %.0fs (host fetched it in %.1fs)"
                   % (LOAD_BUDGET, probe.get("elapsed", -1)))
        gaptext = ""
        if gap:
            gaptext = (" -- and the document asked for %d subresources (%d "
                       "stylesheets, %d script srcs) against %d requests issued, "
                       "short by %d" %
                       (gap["mandatory"], gap["stylesheets"], gap["script_src"],
                        gap["requested"], gap["short_by"]))
        if changed <= BLANK_MAX:
            finish("BLANK", "loaded in %.1fs and painted %d changed pixels "
                            "(%d exceptions)%s" % (load_s, changed, nexc, gaptext))
        if nexc:
            finish("ERRORS", "painted %d changed px in %.1fs but %d JS exception(s)%s"
                   % (changed, load_s, nexc, gaptext))
        if gap:
            finish("GAP", "painted %d changed px in %.1fs and threw nothing, but "
                          "never requested %d of the %d subresources the document "
                          "asks for (%d stylesheets in the document)"
                   % (changed, load_s, gap["short_by"], gap["mandatory"],
                      gap["stylesheets"]))
        # PAINTED, not OK. The word `OK` promises correctness this instrument
        # cannot check: it measures that pixels changed and that nothing threw.
        # deepseek painted, and painted WRONG, and was called OK for it. Nothing
        # here looks at whether the right pixels changed -- reftests do that, and
        # none run on this machine.
        finish("PAINTED", "painted %d changed px in %.1fs, no JS exceptions, no "
                          "subresource gap" % (changed, load_s))

    except SystemExit:
        raise
    except Exception as e:                                        # noqa: BLE001
        import traceback
        traceback.print_exc()
        rec["traceback"] = traceback.format_exc()
        finish("HARNESS", "driver error: %r" % (e,))


if __name__ == "__main__":
    main()
