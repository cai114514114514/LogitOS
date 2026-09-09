#!/usr/bin/env python3
"""document.currentScript, measured IN THE GUEST -- in the shipped browser, on
the real machine, off its own serial log.

    python3 tests/qmp/qmp_currentscript.py <iso> <disk.img> [--expect-null]

WHY THIS EXISTS AND WHY IT IS A BOOT HARNESS RATHER THAN A HOST GATE.

The feature had a host instrument (tests/unit/webapi_probe.c) that measured it
green for months while the shipped browser returned null for every inline
classic script on every page. Not by accident: js_page_begin_script took a
FILENAME and recovered the <script> node by matching the string, and the probe
-- which evaluates each script itself -- passed a string chosen so the match
would succeed, while browser.c passed the page URL + "#inline-script-N" (which
an inline script needs as its import() base) and matched nothing. The inline
test was literally `!strchr(filename, ':')`, and every page URL contains
"https:".

So the instrument was measuring its own naming scheme. Rule 1 in its purest
form, and the reason this file exists at all: the ONLY way to see this was to
put the question inside the guest, in the browser, with browser.c doing the
calling. A host test cannot ask it, because the thing under test is the channel
between the embedder and the runtime, and a host test IS a different embedder.

WHAT IS ASSERTED, and none of it is "the property exists":

  1. an inline classic script gets ITS OWN <script> element        (was null)
  2. TWO inline scripts get TWO DIFFERENT elements -- an implementation
     that returns "the first script", or the last, or the one whose text
     happens to match, passes a one-script test and fails this one
  3. an EXTERNAL script gets its element, and .src is the ABSOLUTE url
  4. src="./rel.js" pairs with the absolute URL it resolved to -- the second
     half of the same defect: the old matcher paired attribute to filename
     with a SUFFIX test, and the literal "./" is in the attribute and not in
     the URL
  5. THE MICROTASK CHECKPOINT IS INSIDE THE SCRIPT AND THE TIMER IS NOT.
     HTML's "execute the script element" sets currentScript, calls "run a
     classic script" -- which performs the microtask checkpoint -- and only
     THEN restores the old value. So a promise reaction queued by a classic
     script sees that script; a setTimeout callback, which runs in a later
     task, sees null. Both halves are asserted, because getting the first one
     wrong is what killed Next.js here (its turbopack runtime reads
     currentScript from an async function after an await, on every
     turbopack-built site on the web) and getting the second one wrong would
     hand a bundler somebody else's script to load its chunks from
  6. `document.currentScript.remove()` -- the x.com idiom, four uncaught
     exceptions on that site -- detaches the tag AND leaves currentScript
     naming it. This is the case the old index-into-document.scripts design
     could not do even with the matcher fixed: removing the element renumbers
     the collection under the index still standing on it
  7. the node reaches the PIXELS: a colour that appears in no stylesheet on
     the page is painted through the element currentScript handed back

--expect-null is the NEGATIVE CONTROL side. It runs the identical page against
a browser whose js_page.c was built with -DJS_CURRENTSCRIPT_NOTOLD (the node is
dropped on the floor -- the shipped defect on a switch) and requires every one
of 1,2,3,4,6,7 to read NULL. If that build ever satisfies the positive
assertions, this harness is measuring something other than this change.
"""

import os
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM      # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
EXPECT_NULL = "--expect-null" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# From the page's own stylesheet ...
RED = (254, 1, 2)
# ... and the one colour only a script holding its OWN node can produce. It
# appears in no stylesheet on the page, so finding it is not "something
# painted" -- it is "this specific element was reached through this specific
# property".
ORANGE = (254, 127, 1)

STYLE = """<style>
html, body { background: #ffffff; margin: 0; padding: 0; color: #000000; }
div { display: block; font-size: 30px; color: #000000; }
#mark { background: #fe0102; }
</style>"""

# The page. Every script prints one line with a fixed prefix, and every line is
# written so it prints NULL rather than throwing when the property is absent --
# a control that dies on a TypeError proves nothing about currentScript.
PAGE = """<!doctype html>
<html><head><title>currentScript</title>""" + STYLE + """</head><body>
<div id="mark">MARKBEFOREMARKBEFOREMARKBEFORE</div>

<script id="s1">
var cs = document.currentScript;
console.log('CS-INLINE-1 ' + (cs ? ('id=' + cs.id + ' tag=' + cs.tagName) : 'NULL'));
window.__c1 = cs;
</script>

<script id="sext" src="/js/ext.js"></script>

<script id="s2">
var cs = document.currentScript;
console.log('CS-INLINE-2 ' + (cs ? ('id=' + cs.id) : 'NULL'));
console.log('CS-DISTINCT ' +
  ((window.__c1 && cs) ? (window.__c1 !== cs ? 'YES' : 'NO-SAME-NODE') : 'NULL'));
/* Not a formality: the node has to be usable AS a node, not just present. */
if (cs) { var d = document.getElementById('mark');
          d.textContent = 'VIA-' + cs.id;
          d.style.backgroundColor = '#fe7f01'; }
Promise.resolve().then(function () {
  console.log('CS-MICROTASK ' +
    (document.currentScript ? ('id=' + document.currentScript.id) : 'NULL'));
});
(async function () { await Promise.resolve(); await Promise.resolve();
  console.log('CS-AWAIT ' +
    (document.currentScript ? ('id=' + document.currentScript.id) : 'NULL'));
})();
setTimeout(function () {
  console.log('CS-TIMER ' + (document.currentScript ? 'NOTNULL' : 'NULL'));
}, 0);
</script>

<script id="s3">
/* The x.com idiom: an inline script deletes its own tag. */
var cs = document.currentScript;
if (!cs) { console.log('CS-REMOVE NULL'); console.log('CS-AFTER-REMOVE NULL'); }
else {
  try { cs.remove(); } catch (e) { console.log('CS-REMOVE THREW ' + e); }
  console.log('CS-REMOVE ' + (cs.parentNode ? 'STILL-ATTACHED' : 'DETACHED'));
  console.log('CS-AFTER-REMOVE ' +
    (document.currentScript === cs ? 'SAME'
      : (document.currentScript ? ('id=' + document.currentScript.id) : 'NULL')));
}
</script>

<script id="srel" src="./js/rel.js"></script>
</body></html>
"""

EXT_JS = ("var cs = document.currentScript;\n"
          "console.log('CS-EXT ' + (cs ? ('id=' + cs.id + ' src=' + cs.src) : 'NULL'));\n")
REL_JS = ("var cs = document.currentScript;\n"
          "console.log('CS-REL ' + (cs ? ('id=' + cs.id + ' src=' + cs.src) : 'NULL'));\n")

FILES = {"/js/ext.js": EXT_JS, "/js/rel.js": REL_JS}

tmp = tempfile.mkdtemp(prefix="qmp_cs_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

requested = []


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.split("?")[0]
        requested.append(self.path)
        if path in ("/cs.html", "/"):
            body, ctype = PAGE, "text/html"
        elif path in FILES:
            body, ctype = FILES[path], "text/javascript"
        else:
            body, ctype = "not found\n", "text/plain"
        raw = body.encode()
        self.send_response(200 if body != "not found\n" else 404)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0" % DISK,
     "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
     "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
     "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
     "-display", "none", "-no-reboot",
     "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
     "-serial", "file:" + serial_path,
     "-qmp", "unix:%s,server,nowait" % qmp_path],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

checks = []


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    for ok, name in checks:
        print("  %s %s" % ("ok  " if ok else "FAIL", name))
    print("----- artefacts in %s -----" % tmp)
    print("----- paths the fixture server was asked for -----")
    print("\n".join(requested[-40:]))
    print("----- every CS- line the guest printed -----")
    for ln in serial().splitlines():
        if ln.startswith("CS-"):
            print("  " + ln)
    print("----- serial (tail) -----")
    print(serial()[-6000:])
    print("-------------------------")
    proc.kill()
    sys.exit(1)


def ck(cond, name):
    checks.append((bool(cond), name))
    print(("ok: " if cond else "FAIL: ") + name)
    if not cond:
        die(name)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


def cs_line(tag):
    """The single 'CS-<tag> ...' line, or None. Exactly one is required: two
    means the page ran twice and every comparison below is between different
    loads."""
    got = [ln.strip() for ln in serial().splitlines()
           if ln.strip().startswith("CS-" + tag + " ")]
    if len(got) > 1:
        die("the guest printed CS-%s %d times -- the page ran more than once, "
            "so nothing here compares one load with itself: %r" % (tag, len(got), got))
    return got[0][len("CS-" + tag) + 1:] if got else None


def goto(ui, url, bar=None):
    if bar:
        ui.click_at(bar[0], bar[1])
    for _ in range(90):
        ui.key("backspace", settle=0.02)
    ui.typ(url)
    ui.key("ret")


try:
    if not wait_serial("LOGIT_BOOT_OK", 240, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 90, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

    ui = Session(qmp_path, serial=serial_path)
    # One click, at the tile the GUEST names for browser.aex, verified against the
    # guest's own [wm] launched line. The re-click loop this replaces was the
    # apology a hand-kept slot constant needed: when the pack list grows, the dock
    # is re-centred, every hard-coded index moves half a slot, and the miss looks
    # exactly like a slow launch. See tests/qmp/qmp_ui.py's dock block.
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(6)

    base = "http://10.0.2.2:%d" % PORT
    goto(ui, base + "/cs.html")

    # The LAST script on the page is the external ./js/rel.js, so waiting for
    # its line waits for the whole classic pass -- and the timer line is what
    # says the event loop turned at least once after it.
    ck(wait_serial("CS-REL ", 120, "the page's scripts"),
       "the fixture page loaded and every one of its five scripts ran")
    ck(wait_serial("CS-TIMER ", 30, "the timer"),
       "the event loop turned after the classic pass (the setTimeout fired)")
    time.sleep(1.5)

    for tag in ("INLINE-1", "INLINE-2", "EXT", "REL", "DISTINCT",
                "MICROTASK", "AWAIT", "TIMER", "REMOVE", "AFTER-REMOVE"):
        ck(cs_line(tag) is not None,
           "the guest printed a CS-%s line at all" % tag)

    print("\n  what the guest said:")
    for tag in ("INLINE-1", "INLINE-2", "EXT", "REL", "DISTINCT",
                "MICROTASK", "AWAIT", "TIMER", "REMOVE", "AFTER-REMOVE"):
        print("    CS-%-13s %s" % (tag, cs_line(tag)))
    print()

    # ---- the rail that must hold on BOTH sides of the control -------------
    # A setTimeout callback runs in a LATER TASK, past the script's own
    # microtask checkpoint, so currentScript is null there whichever way the
    # switch is set. It does not distinguish fixed from broken; it is the rail
    # a fix must not break in the other direction -- an implementation that
    # simply left the node set forever would satisfy every other assertion
    # here and fail this one.
    ck(cs_line("TIMER") == "NULL",
       "currentScript is NULL in a setTimeout callback -- a later task, past "
       "this script's checkpoint")

    if EXPECT_NULL:
        # ---- the negative control -----------------------------------------
        for tag in ("INLINE-1", "INLINE-2", "EXT", "REL", "REMOVE",
                    "AFTER-REMOVE", "MICROTASK", "AWAIT"):
            ck(cs_line(tag) == "NULL",
               "CONTROL: with the node withheld from the runtime, CS-%s is NULL"
               % tag)
        ck(cs_line("DISTINCT") == "NULL",
           "CONTROL: there are no two nodes to be distinct")
        p = PPM(ui.screendump(os.path.join(tmp, "ctl.ppm")))
        ck(p.find_color(ORANGE) is None,
           "CONTROL: the paint that only a script holding its own node can "
           "produce is absent from the screen")
        ck(p.find_color(RED) is not None,
           "CONTROL: and the page DID render -- the stylesheet's own colour is "
           "on screen, so this is currentScript missing and not the page failing")
        print("\nPASS (negative control): with -DJS_CURRENTSCRIPT_NOTOLD every "
              "assertion of the positive gate fails, and the page still renders")
        proc.kill()
        sys.exit(0)

    # ---- 1. an inline classic script gets its own element ------------------
    ck(cs_line("INLINE-1") == "id=s1 tag=SCRIPT",
       "AN INLINE CLASSIC SCRIPT GETS ITS OWN <script> ELEMENT "
       "(got %r) -- this was null in the shipped browser for every inline "
       "script on every page" % cs_line("INLINE-1"))
    ck(cs_line("INLINE-2") == "id=s2",
       "the SECOND inline script gets ITS element, not the first one's "
       "(got %r)" % cs_line("INLINE-2"))
    ck(cs_line("DISTINCT") == "YES",
       "and the two are DIFFERENT NODES -- an implementation that hands out "
       "'the first script' passes a one-script test and fails here")

    # ---- 2. external scripts, absolute src --------------------------------
    ext = cs_line("EXT")
    ck(ext.startswith("id=sext "),
       "an EXTERNAL script gets its own element (got %r)" % ext)
    ck(ext.endswith(" src=%s/js/ext.js" % base),
       "and its .src is the ABSOLUTE url a chunk loader can use as a base "
       "(got %r)" % ext)

    # ---- 3. the './' src, which the old suffix matcher could not pair ------
    rel = cs_line("REL")
    ck(rel.startswith("id=srel "),
       "src=\"./js/rel.js\" pairs with the element it belongs to (got %r) -- "
       "the old matcher compared the attribute to the tail of the absolute "
       "URL, and the literal './' is in the attribute and not in the URL" % rel)
    ck(rel.endswith(" src=%s/js/rel.js" % base),
       "and its .src is the resolved absolute url (got %r)" % rel)

    # ---- 4. the x.com idiom ------------------------------------------------
    ck(cs_line("REMOVE") == "DETACHED",
       "document.currentScript.remove() detaches the tag (got %r)"
       % cs_line("REMOVE"))
    # ---- 4a. the microtask checkpoint, which is INSIDE the script ---------
    ck(cs_line("MICROTASK") == "id=s2",
       "a promise reaction queued by a classic script sees THAT SCRIPT as "
       "currentScript (got %r) -- HTML performs the microtask checkpoint "
       "inside 'run a classic script', before 'execute the script element' "
       "restores the old value. Next.js's turbopack runtime reads it exactly "
       "there, from an async function after an await, on every turbopack site "
       "on the web" % cs_line("MICROTASK"))
    ck(cs_line("AWAIT") == "id=s2",
       "and so does a continuation two awaits deep (got %r) -- the whole "
       "checkpoint drains inside the script's scope, not just the first job"
       % cs_line("AWAIT"))

    ck(cs_line("AFTER-REMOVE") == "SAME",
       "and currentScript still names that element afterwards (got %r) -- the "
       "index-into-document.scripts design could not do this even with the "
       "matcher fixed, because removing the element renumbers the collection "
       "under the index standing on it" % cs_line("AFTER-REMOVE"))

    # ---- 5. the pixels -----------------------------------------------------
    p = PPM(ui.screendump(os.path.join(tmp, "after.ppm")))
    ck(p.find_color(ORANGE) is not None,
       "THE NODE REACHED THE PAINT: a colour no stylesheet on the page "
       "mentions is on screen, written through the element currentScript "
       "handed back")
    ck(p.find_color(RED) is None,
       "and the stylesheet's own colour is gone from that box")

    print("\nPASS: document.currentScript names the running <script> element in "
          "the SHIPPED browser -- inline, external, relative-src, and after the "
          "element removes itself")
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
