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
from qmp_ui import Session, PPM, browser_client_point             # noqa: E402

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
        "console_errors": [],     # page-reported errors, not assumed uncaught exceptions
        "webapi_errors": [],      # fetch failures can be caught by a library
        "fetch_failed": [],
        "fetch_stalled": [],
        "cannot_fetch": [],
        "loader_errors": [],      # dropped jobs / refused decoded resources
        "load_complete_ms": [],   # actual load event, not initial load done
        "image_state": None,      # last confirmed inventory; None is unobserved
        "load_event_pending": None,
        "skipped_scripts": 0,
        "requests": None, "dials": None, "reused": None,
        "modules": None, "modules_failed": None,
        "heap_peak_k": None,
        "load_done": False,
        "page_fetch_failed": None,
        "app_fault": None,
        "panic": False,
        # Guest monotonic phases, unlike load_seconds/paint_seconds below
        # which are host orchestration intervals. Nested module fetching is
        # currently included in scripts_execute, so that is not pure CPU.
        "load_phases_ms": [],
        "module_phases_ms": [],  # exclusive nested phases from the real module loader
        # load done's counters are an INITIAL checkpoint, not the end of
        # dynamic import(). Preserve them and report actual later fetch log
        # observations separately; downloaded bytes do not prove execution.
        "module_fetch_observations": [],
    }
    lines = text.splitlines()
    i = 0
    seen = {}
    image_state = load_pending = None
    while i < len(lines):
        ln = lines[i]
        if "[browser] load: about:images" in ln:
            image_state = load_pending = None
            out["image_state"] = out["load_event_pending"] = None
        if "[load-complete] elapsed_ms=" in ln:
            m = re.search(r"elapsed_ms=(\d+)", ln)
            if m:
                out["load_complete_ms"].append(int(m[1]))
        if "[images] layout " in ln:
            image_state = {k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", ln)}
        if "[images] owed=" in ln:
            m = re.search(r"load_event_pending=(\d+)", ln)
            if m:
                load_pending = bool(int(m[1]))
        if "[images] end state" in ln and (i < len(lines) - 1 or text.endswith(('\n', '\r'))):
            out["image_state"], out["load_event_pending"] = image_state, load_pending
        # 2026-09-13: Doubao was PAINTED beside 86 dropped script messages;
        # Apple was PAINTED despite decoded-image refusal. Keep these as
        # resource diagnostics, separate from page exceptions and network IO.
        if any(s in ln for s in (
                "[browser] inserted-script queue ",
                "[browser] inserted-script drain guard tripped",
                "[browser] inserted script LOST:",
                "[browser] script REFUSED", "[img] REFUSED:", "[layout] REFUSED:")):
            out["loader_errors"].append(ln.strip())
        if "[load-perf]" in ln:
            out["load_phases_ms"].append({k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", ln.split("[load-perf]", 1)[1])})
        if "[module-perf]" in ln:
            out["module_phases_ms"].append({k: int(v) for k, v in re.findall(r"(\w+)=(\d+)", ln.split("[module-perf]", 1)[1])})
        mf = re.search(r"\[js\] module loaded (\d+) bytes: (.+)", ln)
        if mf:
            out["module_fetch_observations"].append({"bytes": int(mf[1]), "url": mf[2]})
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
        # A library can catch a failed fetch and log it instead of letting the
        # script throw. QQ's 2026-09-09 guest logged AxiosError and a response
        # limit failure, while the old parser called that PAINTED/no errors.
        # Keep these channels separate so a site's own error is not mislabeled
        # an engine exception, but never erase it from the verdict.
        if ln.startswith("[error] "):
            out["console_errors"].append(ln[len("[error] "):].strip())
        elif re.match(r'^(?:TypeError|ReferenceError|SyntaxError|RangeError|InternalError|URIError|EvalError|Error):', ln):
            # Some real pages log the caught Error directly with console.log,
            # without the console.error prefix. Qwen's XHR constructor error
            # was visible in serial but absent from its 2026-09-13 JSON.
            out["console_errors"].append(ln.strip())
        elif ln.startswith(("[webapi] fetch: ", "[webapi] prelude failed: ")):
            out["webapi_errors"].append(ln[len("[webapi] "):].strip())
        elif "[js] uncaught in " in ln:
            out["timer_exceptions"].append(ln.split("[js] uncaught in ", 1)[1].strip())
        elif "[browser] module exception in " in ln:
            out["module_exceptions"].append(
                ln.split("[browser] module exception in ", 1)[1].strip())
        elif "[browser] module rejected " in ln:
            out["module_exceptions"].append(
                ln.split("[browser] module rejected ", 1)[1].strip())
        elif "[browser] fetch failed (status " in ln or "[img] fetch failed (status " in ln:
            out["fetch_failed"].append(ln.strip())
        elif "[browser] fetch stalled: " in ln:
            out["fetch_stalled"].append(ln.split("[browser] ", 1)[1].strip())
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


def resource_error_count(guest):
    """Count diagnostics, not unique URLs: a stalled image may log twice.

    The old PAINTED verdict ignored even the fetch_failed list it collected,
    and missed the newer [img] prefix entirely. GitHub's 2026-09-10 snapshot
    therefore said no errors beside four image timeouts. Request counts only
    prove issuance, never delivery. Keep this predicate shared with its gate.
    """
    return sum(len(guest.get(key, [])) for key in
               ("fetch_failed", "fetch_stalled", "cannot_fetch", "loader_errors"))


def latest_painted_text(text):
    """Last completed dump, not the largest/first/partially written one.

    about:text/boxes/images return without navigation in browser.c. A later
    page paint is still the specimen, not an invented diagnostic document.
    The caller retains the old pre-diagnostic photo too: these observations
    are sequential, not an atomic screenshot/DOM snapshot or a usable-page test.
    """
    pairs = re.findall(
        r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte[^\n]*\n"
        r"\[dl\] ---8<--- begin painted text\n(.*?)"
        r"\[dl\] ---8<--- end painted text", text.replace("\r\n", "\n"), re.S)
    if not pairs:
        return {"text_runs": None}
    runs, count, body = pairs[-1]
    return {"text_runs": int(runs), "text_bytes": int(count),
            "text": "\n".join(ln[5:] for ln in body.splitlines() if ln.startswith("[dl] "))}


def diagnostic_progress(text, word):
    """Arrival is not a complete census, and an older dump is not this one.

    Bilibili's three native triggers were logged after the old six-second
    observation budget. Even a load marker only proves dispatch: a large box
    dump can still be printing. Match a complete terminator AFTER the latest
    exact trigger, never an old automatic paint or a partially written line.
    Older guests without the images terminator remain explicitly incomplete.
    Input tracing has an armed line, not a display-list terminator. Omitting
    that command from this table raised KeyError('input') AFTER the real guest
    armed tracing and discarded the site's completed load observations.
    """
    endings = {'text': r'\[dl\] ---8<--- end painted text',
               'boxes': r'\[dl\] ---8<--- end boxes \(\d+ shown of \d+\)',
               'images': r'\[images\] end state',
               'input': r'\[input-trace\] armed [^\r\n]*'}
    end = endings[word]
    starts = list(re.finditer(r'^\[browser\] load: about:' + re.escape(word) +
                             r'\r?\n', text, re.M))
    return {'arrived': bool(starts),
            'completed': bool(starts and re.search('^' + end + r'\r?\n',
                               text[starts[-1].end():], re.M))}


def observed_input_box(text, element_id=None, element_class=None):
    """One positive-sized ordinary control in the last completed box dump.

    printf pads height to four characters: the first input probe assumed one
    space before '<textarea>' and missed a real 766x42 box. Do not fall back
    to guessed coordinates or pick the first of ambiguous duplicate IDs.
    """
    dumps = re.findall(r'\[dl\] ---8<--- begin boxes\r?\n(.*?)\[dl\] ---8<--- end boxes', text, re.S)
    if not dumps:
        return None
    if bool(element_id) == bool(element_class):
        return None
    target = ('#' + re.escape(element_id) if element_id else
              r'(?:#[^\s]+\s+)?\.' + re.escape(element_class))
    pattern = (r'\[dl\] ctrl\s+(-?\d+),\s*(-?\d+)\s+(\d+)x\s*(\d+)\s+'
               r'<(?:input|textarea)> ' + target + r'(?:\s|$)')
    boxes = re.findall(pattern, dumps[-1])
    if len(boxes) != 1:
        return None
    box = tuple(map(int, boxes[0]))
    return box if box[2] > 0 and box[3] > 0 else None


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


def native_input_progress(text):
    """Do not confuse a key's default action with its subsequent frame."""
    key = '[input-trace] key-default '
    frame = '[input-trace] frame-painted '
    return {'native_defaults_observed': text.count(key),
            'paint_after_defaults_observed': text.rfind(frame) > text.rfind(key) >= 0}


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
        # AN ERROR RESPONSE HAS A BODY, AND ON THIS CORPUS IT IS OFTEN THE ONLY
        # INTERESTING ONE. This branch used to record `bytes=0` and drop
        # `e.read()` unread, which was not a small omission: a Cloudflare
        # managed challenge IS an HTTP 403, so EVERY challenge page this
        # harness has ever been served was filed as "0 bytes" with its actual
        # content discarded. The evidence sat in the tree unexamined --
        # tests/scoreboard/full-corpus/openai.json reads
        # `"host": {"ok": true, "status": 403, "bytes": 0}` and there is no
        # openai.host.html beside it, while every 200 row has one.
        #
        # What that cost, concretely: a claim that Cloudflare's challenge ships
        # a WebAssembly module went unchecked for a day and was told to the
        # owner, because the one instrument that could have settled it threw
        # the bytes away. Fetched by hand instead, the answer is zero
        # occurrences of WebAssembly in 341 KB of real challenge code -- and
        # the thing that DOES break the page (`contentDocument.createElement`)
        # is one grep away in those same bytes.
        #
        # `ok=True` is kept: the request succeeded and the server answered.
        # A 403 is a measurement, not a harness failure -- which is exactly
        # why its body has to be kept.
        raw = b""
        try:
            raw = e.read()
        except Exception:                                         # noqa: BLE001
            pass
        # THE SAME DECODE, THE SAME GUNZIP AND THE SAME COUNTS AS THE 200 PATH,
        # and the two-line version of this branch was wrong twice over. It kept
        # `e.read()` as RAW BYTES while the 200 path stores a decoded str, so
        # the caller's `fh.write(hb)` into a text handle raised
        # `TypeError: write() argument must be str, not bytes` and the whole
        # run came back as verdict HARNESS -- i.e. the fix that was made so a
        # challenge body would finally be kept made every 403 measure NOTHING
        # AT ALL, which is strictly worse than the bytes=0 it replaced.
        # Watched happening on the first openai run after it landed.
        # It also hard-coded script_tags=0/script_src=0/inventory=None, so even
        # once the body was kept, every COUNT taken from it read zero -- on the
        # one document in this corpus whose script inventory is the entire
        # question. An error body is not a lesser body; it is the body.
        if raw and e.headers is not None and e.headers.get("Content-Encoding", "") == "gzip":
            try:
                raw = gzip.GzipFile(fileobj=io.BytesIO(raw)).read()
            except OSError:
                pass
        body = raw.decode("utf-8", "replace")
        result.update(ok=True, status=e.code, final_url=url, bytes=len(raw),
                      redirects=chain,
                      script_tags=len(re.findall(r"<script\b", body, re.I)),
                      script_src=len(re.findall(r"<script\b[^>]*\bsrc=", body, re.I)),
                      img_tags=len(re.findall(r"<img\b", body, re.I)),
                      inventory=inventory(body),
                      elapsed=round(time.time() - t0, 1))
        if body:
            result["_body"] = body
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
    ap.add_argument("--memory", type=int, default=1024,
                    help="guest RAM in MiB; default matches make run (1024)")
    ap.add_argument("--name", required=True)
    ap.add_argument("--url", required=True)
    ap.add_argument("--out", required=True, help="where to write the JSON record")
    ap.add_argument("--shots", default=None, help="directory for the screenshots")
    ap.add_argument("--keep", action="store_true", help="keep the serial log and PPMs")
    # OFF by default and that is not timidity: the display list of a real page
    # is thousands of lines and the serial console is the same one every other
    # measurement in this file arrives on. An instrument that floods the log it
    # writes to has replaced the thing it was measuring -- the same rule the
    # painted-text dump states about its own auto-print. Turn it on for one
    # site when the question is "how wide does layout think that box is".
    ap.add_argument("--boxes", action="store_true",
                    help="also dump the display list (about:boxes) -- verbose")
    ap.add_argument("--images", action="store_true",
                    help="also request decoded image and pending transfer diagnostics")
    ap.add_argument("--sample-registers", action="store_true",
                    help="diagnostic only: sample guest CPU registers during slow initial load")
    input_target = ap.add_mutually_exclusive_group()
    input_target.add_argument("--input-id", help="ordinary input/textarea ID from the latest box dump; no scrolling or submit")
    input_target.add_argument("--input-class", help="first reported class of one unique ordinary input/textarea in the box dump")
    ap.add_argument("--input-text", help="literal text to type, without newline; requires one input selector")
    ap.add_argument("--trace-input", action="store_true", help="arm bounded native hit/focus diagnostics before explicit input attempt")
    ap.add_argument("--diagnostic-wait", type=float, default=25.0,
                    help="host observation budget per diagnostic trigger; never a guest performance measurement")
    args = ap.parse_args()
    if args.diagnostic_wait <= 0 or args.diagnostic_wait > 60:
        ap.error('--diagnostic-wait must be in (0, 60] seconds')
    if bool(args.input_id or args.input_class) != (args.input_text is not None) or (args.input_text and any(c in args.input_text for c in '\r\n')):
        ap.error('one input selector and --input-text must be paired; multiline/submitting input is not supported')

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
           "-snapshot", "-m", str(args.memory) + "M", "-smp", "4", "-accel", "tcg,thread=multi",
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
        # One click at the tile the GUEST names for browser.aex, verified
        # against the guest's own [wm] launched line. The scoreboard boots one
        # machine per live site; a coordinate that opened the wrong app would
        # have published every site's numbers as browser findings.
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish("HARNESS", str(e))
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
        #
        # THE URL IS CONFIRMED BEFORE ANYTHING IS MEASURED, and it was not
        # until 2026-08-25. about() thirty lines below has retried and
        # confirmed against the browser's own `[browser] load:` line since it
        # was written; the SITE url -- the one every published verdict is
        # about -- was typed once and never checked. Two standards in one
        # file, and the unchecked one is the load-bearing half.
        #
        # What that cost, from tests/scoreboard/0820-g4b/deepseek#2: the
        # harness typed https://www.deepseek.com/ and the guest received
        #     [browser] load: https://www.deepseek.c/
        # -- two characters lost to the unpaced key_shift this commit also
        # fixes -- and the row was PUBLISHED as a browser FETCH-FAIL against
        # a host that does not exist. An instrument that fabricates a finding
        # about the thing it exists to measure is worse than no instrument.
        #
        # A mistyped URL is recorded as HARNESS, never as a browser verdict.
        # That distinction is the whole point: "the browser failed", "the
        # harness failed" and "the site refused us" are three findings and
        # only the first one is ours.
        typed = None
        for attempt in range(3):
            mark = len(serial())
            ctrl(ui, "l")
            ui.typ(args.url)
            t0 = time.time()
            ui.key("ret")
            if not wait_for("[browser] load: ", 8.0, mark):
                continue                      # the bar never took it at all
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
            finish("HARNESS", "the URL never reached the address bar intact"
                              " (typed %r, got %r)"
                              % (args.url, rec.get("url_mistypes")))
        rec["url_confirmed"] = typed
        rec["url_attempts"] = attempt + 1

        loaded = False
        fetch_failed = False
        register_sample_at = t0 + 20
        while time.time() - t0 < LOAD_BUDGET:
            if args.sample_registers and time.time() >= register_sample_at:
                regs = ui.cmd({"execute": "human-monitor-command",
                               "arguments": {"command-line": "info registers -a"}})
                rec.setdefault('load_register_samples', []).append(regs)
                # Persist before completion: a stuck guest may never finish.
                with open(os.path.join(shots_dir, args.name + '.registers.json'), 'w') as fh:
                    json.dump(rec['load_register_samples'], fh, indent=2)
                register_sample_at = time.time() + 10
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
        # TYPED AND THEN CONFIRMED, because typing is not arriving.
        #
        # MEASURED: `about:text` reached the guest on every run until the
        # canvas context landed, and on none afterwards -- 0818-b has it,
        # 0818-c and 0818-cv do not. Nothing about the trigger changed. What
        # changed is that stripe now renders its real page instead of an error
        # boundary, so the load runs 31 s, the paint budget expires before the
        # page settles, and the keystrokes go into a browser that is still
        # inside a blocking fetch -- against a PS/2 controller with a one-byte
        # buffer. The column kept working only because the painted-text dump
        # needs no trigger and prints itself; about:boxes has no such fallback
        # and would simply have been missing, silently.
        #
        # So the trigger is confirmed against the log line the browser prints
        # for every load, and retried if it did not arrive. The outcome is
        # RECORDED either way: "the dump did not happen" and "the page painted
        # nothing" are different findings and only the first is the harness's.
        def about(word, settle, tries=3):
            state = {'arrived': False, 'completed': False}
            for _ in range(tries):
                frm = len(serial())
                ctrl(ui, "l")
                ui.typ("about:" + word)
                ui.key("ret")
                # Six seconds marked all three Bilibili text triggers absent,
                # yet their exact load lines appeared later in the same log.
                # Give the pending native turn time to arrive before retrying;
                # preserve a bounded failure, not a synthetic successful dump.
                deadline = time.monotonic() + args.diagnostic_wait
                while time.monotonic() < deadline:
                    state = diagnostic_progress(serial(frm), word)
                    if state['completed']:
                        break
                    time.sleep(0.2)
                if state['arrived']:
                    # Do not queue another command when dispatch was seen but
                    # output has not completed. Preserve the old arrival field
                    # and add the stronger fact, without redefining old JSON.
                    rec['about_' + word + '_completed'] = state['completed']
                    time.sleep(settle)
                    return True
                time.sleep(1.0)
            rec['about_' + word + '_completed'] = False
            return False

        rec["about_text_arrived"] = about("text", 2.5)

        # The display list, on request only. Same channel, same
        # does-not-navigate property, and after about:text so that a run with
        # both gives the words first and then their boxes -- which is the
        # order the two questions are actually asked in.
        if args.boxes or args.input_id or args.input_class:
            rec["about_boxes_arrived"] = about("boxes", 3.0)
        if args.images:
            rec["about_images_arrived"] = about("images", 1.0)

        # Correction to the old "settled = finished" claim above: a blank
        # frame can stay stable while a synchronous late module compiles.
        # z.ai's 10,589,862-byte module and chat-input boxes arrived AFTER the
        # old primary photo. Preserve that photo/score, then take a second
        # observation after the confirmed diagnostic turns. A later screenshot
        # still is not proof that a page is quiescent or interactively usable.
        late_ppm = os.path.join(tmp, "late.ppm")
        ui.goto(*PARK)
        ui.screendump(late_ppm, settle=0.8)
        late = PPM(late_ppm)
        late_png = os.path.join(shots_dir, "%s.late.png" % args.name)
        ppm_to_png(late_ppm, late_png)
        rec["late_observation"] = dict(
            latest_painted_text(serial(mark)), shot=late_png,
            changed_px=changed_pixels(base, late),
            initial_to_late_changed_px=changed_pixels(after, late),
            basis="after diagnostic turns; not a completion or usability guarantee")

        if args.input_id or args.input_class:
            # Explicit opt-in only; never press Enter, solve a challenge or
            # silently choose a field. The untouched photo above remains the
            # scored observation; this later artifact records a native attempt.
            try:
                if args.trace_input:
                    trace_mark = len(serial())
                    if not about('input', 0.5):
                        raise AssertionError('native input trace command did not arrive')
                    if not wait_for('[input-trace] armed', 3.0, trace_mark):
                        raise AssertionError('browser did not arm input tracing')
                box = observed_input_box(serial(mark), args.input_id, args.input_class)
                if box is None:
                    raise AssertionError('input selector did not identify one observed positive-sized control')
                x, y, w, h = box
                point = browser_client_point(serial(), x + w/2, y + h/2)
                ui.click_at(*point)
                input_mark = len(serial())
                ui.typ(args.input_text)
                if args.trace_input:
                    # A one-second photo saw mouse-up but no EV_KEY yet on a
                    # busy page. Observe actual native defaults, not "QMP sent
                    # the bytes". This bounded host wait is instrumentation,
                    # never a guest input-latency/performance measurement.
                    # A default can still precede callbacks and painting in
                    # the same event-loop turn. Require the later paint marker
                    # too; the screenshot remains the presentation evidence.
                    deadline = time.monotonic() + 45
                    while time.monotonic() < deadline:
                        progress = native_input_progress(serial(input_mark))
                        if progress['native_defaults_observed'] >= len(args.input_text) and progress['paint_after_defaults_observed']:
                            break
                        time.sleep(0.2)
                input_ppm = os.path.join(tmp, 'input.ppm')
                ui.screendump(input_ppm, settle=1.0)
                input_png = os.path.join(shots_dir, '%s.input.png' % args.name)
                ppm_to_png(input_ppm, input_png)
                rec['native_input_attempt'] = {'id': args.input_id, 'class': args.input_class, 'text': args.input_text,
                    'point': point, 'shot': input_png, 'submitted': False,
                    'basis': 'key events sent; inspect screenshot for actual field contents'}
                if args.trace_input:
                    rec['native_input_attempt'].update(native_input_progress(serial(input_mark)))
                if args.boxes:
                    # Preserve the input photo FIRST. A newly focused modal
                    # may not exist in the pre-input boxes; its actual layout
                    # distinguishes hidden UI from a focus-routing failure.
                    box_mark = len(serial())
                    requested = about('boxes', 0.5)
                    rec['native_input_attempt']['post_input_boxes_completed'] = bool(
                        requested and wait_for('[dl] ---8<--- end boxes', 10.0, box_mark))
            except AssertionError as e:
                # An optional diagnostic miss must not erase the completed
                # load/paint evidence or silently look like successful typing.
                rec['native_input_attempt'] = {'id': args.input_id, 'class': args.input_class, 'sent': False,
                    'reason': str(e), 'submitted': False}

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

        # THE COUNT AND THE WORDS MUST COME FROM THE SAME DUMP, AND FOR MONTHS
        # THEY DID NOT. The count was findall(...)[-1] -- the LAST summary, on
        # the reasoning quoted above that the first is an early frame -- while
        # the words were re.search(...), which returns the FIRST block. Two
        # doors on one jar, inside one instrument, and it reported a number
        # from one paint beside the text of another.
        #
        # It stayed invisible because it always returned A number. Measured
        # 2026-08-29: github reported 3 runs / 14 bytes and had actually
        # painted 89 / 467; kimi reported 5 / 59 and had painted 30 / 188. The
        # scoreboard's whole reason to exist -- `changed px` cannot tell a
        # rendered page from a flat dark block, so count the WORDS -- was
        # reading the wrong paint on every row.
        #
        # AND THE "TAKE THE LAST" RULE BROKE WHEN THE DUMP COUNT CHANGED. It
        # was right when a run produced two dumps and the first was the empty
        # tab. A run now produces three: the page's early frame, the page
        # settled, and then about:text's OWN render after the harness navigates
        # to it -- and about:text paints a handful of runs of its own. So the
        # last summary in the log is the diagnostic page describing itself.
        #
        # The boundary is not a heuristic about sizes: it is the navigation.
        # Everything before `[browser] load: about:text` is the page under
        # test; everything after is the instrument. Take the LAST dump before
        # that line, and take its count and its body together.
        # Correction (2026-09-10): browser.c now proves these about commands
        # return without navigation. Keep the historical pre-diagnostic fields
        # for comparison, but late_observation above includes the later page
        # paints that this cutoff previously discarded.
        cut = tail_n.find("[browser] load: about:text")
        page_part = tail_n[:cut] if cut >= 0 else tail_n
        pairs = re.findall(
            r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte[^\n]*\n"
            r"(?:\[dl\] ---8<--- begin painted text\n(.*?)"
            r"\[dl\] ---8<--- end painted text)?",
            page_part, re.S)
        m = pairs[-1] if pairs else None
        # Preserve the disappearance too: a final search bar can meet PAINTED's
        # pixel threshold while all result text present earlier has vanished.
        # This is an observation, not a site-specific pass/fail threshold.
        rec["paint_text_history"] = [{"runs": int(p[0]), "bytes": int(p[1])} for p in pairs]
        if m:
            rec["text_runs"] = int(m[0])
            rec["text_bytes"] = int(m[1])
            if m[2]:
                lines = [ln[5:] for ln in m[2].splitlines()
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
        if g["console_errors"] or g["webapi_errors"]:
            finish("ERRORS", "painted %d changed px, with %d page-reported error(s) "
                   "and %d fetch error(s)%s" %
                   (changed, len(g["console_errors"]), len(g["webapi_errors"]), gaptext))
        if resource_error_count(g):
            finish("ERRORS", "painted %d changed px, with %d resource failure diagnostics%s"
                   % (changed, resource_error_count(g), gaptext))
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
