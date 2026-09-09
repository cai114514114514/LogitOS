#!/usr/bin/env python3
"""Generate the webaccel fixtures (the pages the open-time timeline is taken on).

WHY SYNTHETIC, AND WHY THESE SIZES. The mission is "measure where open-time
goes, then fix the biggest levers" -- on a page whose byte counts and
subresource mix are KNOWN, so a phase's cost can be attributed to a number of
bytes rather than to a site's Tuesday deploy. The shape is not invented: it is
the measured shape of the heavy specimens in tests/scoreboard/full-corpus/
(2345.host.html: 171 KB document, 12 external scripts, 33 requests total;
anthropic.host.html: 190 KB document). A replay of a real specimen is measured
SEPARATELY by tests/qmp/qmp_replay.py for the diagnosis half; this fixture is
the REPRODUCIBLE one the gate runs on, because qmp_replay refuses subresources
(404s -- it is a crash-diagnosis harness and its docstring says why) and a
cache gate needs real subresource bytes to have something to cache.

DETERMINISTIC ON PURPOSE: seeded PRNG, fixed output sizes, no clock, no
network. Two runs of this script must produce byte-identical fixtures or the
gate's before/after numbers compare different pages.

The pages self-stamp with performance.now() via console.log (WA-T0 in the
first head script, WA-END in the last body script) -- the guest's own
monotonic clock, on the serial console, so the driver never measures host
wall time. See tests/qmp/qmp_webaccel.py for how the stamps are read.
"""
import os
import random
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
rnd = random.Random(20260830)


def png_rgb(width, height, seed):
    """A small deterministic PNG: gradient + 8x8-block noise.

    Pure zlib/struct, no PIL -- the host may not have it and a fixture
    generator that needs a package is a fixture nobody can regenerate.

    Per-8x8-block noise, not per-pixel: measured, per-pixel noise made a
    240x170 PNG compress to 116-177 KB (six times the ~20 KB a real photo-ish
    thumbnail runs), which silently turned the fixture into an image-heavy
    page it never claimed to be. Block noise keeps local structure (so the
    decoder does real filtering work) at one-eighth the entropy.
    """
    noise = random.Random(seed)
    blocks_w = (width + 7) // 8
    rows = []
    for by in range((height + 7) // 8):
        brow = [noise.randrange(256) for _ in range(blocks_w)]
        for _ in range(8):
            row = bytearray()
            for bx in range(blocks_w):
                n = brow[bx]
                for x in range(8):
                    px = bx * 8 + x
                    if px >= width:
                        break
                    row += bytes(((px * 255) // max(1, width - 1),
                                  (by * 8 * 255) // max(1, height - 1),
                                  n))
            rows.append(bytes(row))
    # Trim the last block-row's overshoot, then prefix each row with filter 0.
    raw = b"".join(b"\x00" + r[:width * 3] for r in rows[:height])

    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    ihdr = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", ihdr)
            + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def css_block(n, size):
    """~`size` bytes of plausible CSS: varied selectors, real properties.

    Each emitter consumes exactly one (i, k) pair and returns a declaration, so
    the property list can be heterogeneous without a %-tuple that has to agree
    with every format string at once (the bug this comment replaces).
    """
    def emitters(i, k):
        return [
            "margin:0 0 %dpx %dpx" % ((i * 3 + k) % 40, (i * 7 + k) % 24),
            "padding:%dpx %dpx" % ((i + k) % 20, (i + 2 * k) % 16),
            "color:#%02x%02x%02x" % ((i + k) % 256, (i * 5 + k) % 256, (i + 2 * k) % 256),
            "background-color:#%02x%02x%02x" % ((i * 7 + k) % 256, (i + k) % 256, (i + 3 * k) % 256),
            "font-size:%dpx" % (12 + (i + k) % 20),
            "line-height:1.%d" % ((i + k) % 10),
            "border-radius:%dpx" % ((i + k) % 16),
            "display:flex;flex-direction:%s" % ("row" if (i + k) % 2 else "column"),
            "opacity:0.%d" % ((i + k) % 10),
            "transform:translate%d(%dpx)" % (1 if (i + k) % 3 else -1, (i + k) % 30),
        ]
    out = []
    total = 0
    i = 0
    while total < size:
        sel = ".c%d-%d .item:nth-child(%d), .c%d .deep > span[data-i=\"%d\"]" % (
            i % 7, i % 13, (i % 5) + 1, (i + 3) % 7, i % 97)
        em = emitters(i, 0)
        body = ";".join(em[(i + k) % len(em)] for k in range(4))
        rule = "%s{%s}\n" % (sel, body)
        out.append(rule)
        total += len(rule)
        i += 1
    return "".join(out)


def js_module(n, size):
    """~`size` bytes of plausible JS: long minified-ish lines + a real tail.

    The tail does actual work (a few hundred thousand simple ops + one DOM
    append) so the "scripts run" phase is not measuring an empty interpreter
    loop. Everything else is dead-ish weight, which is what a real bundle's
    bytes mostly are on a page that only calls a fraction of its library.
    """
    head = "var m%d=(function(){'use strict';var cache={};\n" % n
    body = []
    total = 0
    i = 0
    while total < size - 900:
        line = ("cache[%d]=function(a,b){return (a*%d+b*%d)%%1000003===0?"
                "a+b:Math.max(a-%d,b+%d);};var v%d=cache[%d](%d,%d);%s" % (
                    i, (i % 97) + 1, (i % 31) + 1, i % 13, i % 7, i,
                    i, (i * 11) % 1000, (i * 17) % 1000,
                    "" if i % 17 else "void " + str(i) + ";"))
        body.append(line)
        total += len(line)
        i += 1
    tail = ("""
var s=0;for(var j=0;j<200000;j++){s=(s*31+j%%%d)%%4294967296;}
var d=document.createElement('div');d.className='wa-tail wa-%d';
d.setAttribute('data-s',String(s));document.body.appendChild(d);
return {v:s,cls:'m%d'};})();
""" % (n + 3, n, n))
    return head + "".join(body) + tail


def html_body(n_sections):
    """~n_sections hundred elements of plausible markup."""
    parts = []
    for s in range(n_sections):
        parts.append('<section class="c%d" id="s%d"><h2>Section %d</h2>' % (s % 7, s, s))
        for i in range(20):
            parts.append(
                '<div class="item" data-i="%d"><span class="deep" data-j="%d">'
                'row %d text content for layout measurement %d</span>'
                '<em>note %d</em><p>paragraph %d with enough words to shape '
                'and lay out across a line or two at 16px, so the layout '
                'engine does the work a real page asks of it.</p></div>'
                % (i, i * 3, i, s * 100 + i, i, i))
        parts.append("</section>")
    return "".join(parts)


def write(path, data):
    mode = "wb" if isinstance(data, bytes) else "w"
    with open(os.path.join(HERE, path), mode) as fh:
        fh.write(data)
    print("wrote %-24s %8d bytes" % (path, len(data)))


# ---- the CONTROL page: one small sheet, one small script, one small image ----
# Its purpose in the timeline table is the floor: everything a heavy page spends
# beyond this is the thing being ranked. No WA-* scripts here -- the control is
# not gated, only reported.
write("control.css", css_block(1, 1024))
write("control.js", js_module(0, 2048))
write("control.png", png_rgb(64, 48, 7))
write("control.html", """<!doctype html>
<html><head><meta charset="utf-8"><title>wa-control</title>
<script>console.log('WA-T0 ' + performance.now());</script>
<link rel="stylesheet" href="control.css">
<script src="control.js"></script>
</head><body>
<h1>webaccel control</h1>
<img src="control.png" alt="c">
<p>control page for the open-time timeline.</p>
<script>console.log('WA-END ' + performance.now());</script>
</body></html>
""")

# ---- the HEAVY page: apple-shaped (254 KB doc in full-corpus; 2345 is the
# 12-script/33-request mix reference) ----
sheets = ["heavy-%d.css" % i for i in range(3)]
scripts = ["heavy-%02d.js" % i for i in range(10)]
imgs = ["heavy-%d.png" % i for i in range(8)]
for i, s in enumerate(sheets):
    write(s, css_block(i, 30 * 1024))
for i, s in enumerate(scripts):
    write(s, js_module(i + 1, 40 * 1024))
for i, s in enumerate(imgs):
    write(s, png_rgb(240 + 8 * i, 170 + 6 * i, 100 + i))

# ---- the HEAVY page: 2345-shaped (171 KB doc, 10 scripts + 3 sheets) ---------
# THE DISPLAY-LIST CAP SHAPES THIS PAGE, and that is a measured fact, not a
# style choice: layout.c's additem() refuses silently once nitem hits MAXITEM
# (16384), and every WORD on this page is one item. The first draft had 45
# sections (~30K word-items by the painted-run count) and its eight <img>s --
# last in document order -- never received display-list items at all: no
# [img] line, no image requests, zero serial evidence of why (found with
# img8.html below: the same images on a five-section page decode 8/8). The
# images therefore sit BEFORE the sections here, and the section count stays
# at 10 (~10K items worst case, well under the cap); the byte weight the
# 2345 shape wants comes from a trailing comment, which the tokenizer still
# has to chew (parse cost, the thing being measured) while costing no
# display-list entries. THE SILENT TRUNCATION ITSELF IS AN ENGINE DEFECT --
# the webaccel report carries the loud-refusal patch for layout.c:360.
head_scripts = scripts[:4]
body_scripts = scripts[4:]
doc = []
doc.append("""<!doctype html>
<html><head><meta charset="utf-8"><title>wa-heavy</title>
<script>console.log('WA-T0 ' + performance.now());</script>
""")
doc.extend('<link rel="stylesheet" href="%s">\n' % s for s in sheets)
doc.extend('<script src="%s"></script>\n' % s for s in head_scripts)
doc.append("</head><body>\n")
doc.append("<h1>webaccel heavy specimen</h1>\n")
doc.extend('<img src="%s" alt="i%d">\n' % (s, i) for i, s in enumerate(imgs))
# 10 sections of ~3.6 KB; the comment pad below brings the doc to ~171 KB.
doc.append(html_body(10))
pad_need = 171 * 1024 - sum(len(x) for x in doc) - 80
doc.append("<!-- specimen filler (display-list-free byte weight): "
           + ("x" * max(1024, pad_need)) + " -->\n")
doc.extend('<script src="%s"></script>\n' % s for s in body_scripts)
doc.append("""<script>console.log('WA-END ' + performance.now());</script>
</body></html>
""")
write("heavy.html", "".join(doc))
print("heavy doc: %d bytes (2345 specimen: 171 KB)" % sum(len(x) for x in doc))

# ---- img8: the heavy page's IMAGE BLOCK on a SMALL page --------------------
# A diagnosis fixture, not a specimen: heavy.html's eight <img>s never reach
# layout's image queue (no [img] line, no image requests in any measured run),
# and this page exists to split "the markup is wrong" from "the engine drops
# images once the page is big": same images, same position-after-sections
# shape, five sections instead of forty-five. If images load here and not on
# heavy.html, the bug is size-dependent and belongs to the engine, not the
# fixture.
img8 = ['<!doctype html>\n<html><head><meta charset="utf-8"><title>wa-img8</title>\n',
        '<link rel="stylesheet" href="heavy-0.css">\n',
        # The WA-END stamp so the driver's capture can wait for it like any
        # other page; without it navigate_and_capture() would time out.
        '<script>console.log("WA-END " + performance.now());</script>\n',
        '</head><body>\n',
        "<h1>webaccel img8</h1>\n"]
img8.append(html_body(5))
img8.extend('<img src="%s" alt="i%d">\n' % (s, i) for i, s in enumerate(imgs))
img8.append('<script>console.log("WA-END " + performance.now());</script>\n')
img8.append("</body></html>\n")
write("img8.html", "".join(img8))

# ---- rv: the REVALIDATION specimen ------------------------------------------
# Every subresource name starts with rv-, the ONE prefix the driver's server
# answers with Cache-Control: max-age=1 + a strong ETag (and 304 to a matching
# If-None-Match). Navigated once, waited past 1 s, navigated again, the second
# load MUST show conditional GETs answered 304: rvs>0 in the loadend line,
# dials >0 (the network really was asked), and the page still complete
# (WA-END fires -- the 304s were dressed up as the bytes they vouch for).
write("rv-a.css", css_block(11, 8 * 1024))
write("rv-b.js", js_module(21, 8 * 1024))
write("rv.html", """<!doctype html>
<html><head><meta charset="utf-8"><title>wa-rv</title>
<script>console.log('WA-T0 ' + performance.now());</script>
<link rel="stylesheet" href="rv-a.css">
<script src="rv-b.js"></script>
</head><body>
<h1>webaccel revalidation</h1>
<script>console.log('WA-END ' + performance.now());</script>
</body></html>
""")

# ---- chl: the CHALLENGE-AND-RELOAD specimen (webaccel, 2026-09-02) ---------
# The douyin shape (CLAUDE.md's "the loop, measured on the guest"): a page
# with NO Cookie header answers with a challenge that sets a cookie -- BOTH
# ways a real WAF does it, a Set-Cookie header AND document.cookie from
# script, so the fixture exercises whichever path a browser implements -- and
# re-navigates to the SAME url via location.href. The re-navigation carries
# the cookie the challenge just set.
#
# ONE url, TWO bodies: qmp_webaccel.py's Serve.do_GET branches THIS path on
# the REQUEST'S Cookie header alone (mirroring the rv- server-side branch
# above), choosing between the two bodies below. That is what makes the
# corner meaningful for the cache-key fix: a url-only key cannot serve the
# right body here, because the same url must legitimately answer two
# different requests two different ways.
#
# WHY location.reload() AND NOT location.href = location.href (SUSPECT THE
# APPARATUS FIRST, AGENTS.md rule 1): the first draft of this fixture used
# href-reassignment, and it reproduced NOTHING -- js_webapi.c's loc_set()
# treats an href write that differs from the current document only in ways
# same_document() cannot see (i.e. not at all, since the string is byte-
# identical) as a same-document fragment change and returns without ever
# setting g_have_pending_nav, so the "reload" silently no-oped and the driver
# hung waiting for a second navigation that could never come. location.reload()
# (loc_reload in js_webapi.c) has no such shortcut -- it unconditionally
# schedules g_loc_raw as a pending navigation -- and it is also the more
# faithful reproduction: a JS PoW shell calls reload() (or an equivalent
# self-navigation), not a same-value href write, to ask the browser to ask
# the server again now that the proof cookie is set.
#
# THE MARKERS (WA-CHL-PAGE A / B) are what the driver counts, not WA-END --
# page A deliberately does NOT stamp WA-END: reload() is a navigation and
# this engine's load() does not promise a script survives past requesting
# one (real browsers do not either). Waiting on a stamp that could race the
# navigation is exactly the kind of instrument this tree has been burned by
# -- the marker line, printed BEFORE reload(), has no such race.
write("chl-shell.html", """<!doctype html>
<html><head><meta charset="utf-8"><title>wa-chl-a</title>
<script>
console.log('WA-T0 ' + performance.now());
console.log('WA-CHL-PAGE A');
document.cookie = 'chl=1';
location.reload();
</script>
</head><body><h1>challenge</h1></body></html>
""")
write("chl-real.html", """<!doctype html>
<html><head><meta charset="utf-8"><title>wa-chl-b</title>
<script>
console.log('WA-T0 ' + performance.now());
console.log('WA-CHL-PAGE B');
console.log('WA-END ' + performance.now());
</script>
</head><body><h1>real page</h1></body></html>
""")
