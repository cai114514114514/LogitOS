#!/usr/bin/env python3
"""Which regex in Cloudflare's Turnstile loader does this engine refuse?

    python3 tests/qmp/qmp_regex_turnstile.py <iso> <disk.img>

WHY THIS EXISTS. Measured 2026-08-29 on the real machine: with the canvas
readback landed, https://nowsecure.nl/ now fetches
`challenges.cloudflare.com/turnstile/v0/b/<build>/api.js` over a verified TLS
chain and EXECUTES it -- and the script dies at top level with

    SyntaxError: invalid escape sequence in regular expression
        at RegExp (native)
        at <anonymous> (https://challenges.cloudflare.com/.../api.js)

That is the next wall after toDataURL, and "a regex somewhere in 84 KB of
minified code" is not a work order. This file turns it into one.

The seven RegExp() constructor calls in that build were extracted from the
shipped bytes with a host script and are reproduced below VERBATIM. Each is
compiled on its own inside its own try/catch, so the answer is not "one of them
fails" but WHICH, and the six that compile are just as much of the measurement
as the one that does not.

THE HYPOTHESIS THIS TESTS, written before the run so the run can refute it.
`third_party/quickjs/libregexp.c:728` decides whether a backslash escape is
tolerated with

    strchr("^$\\\\.*+?()[]{}|/", *p)

and `get_class_atom()` (:655) takes an `inclass` argument that this line never
consults. ECMA-262's grammar is
`ClassEscape[U] :: b | [+U] - | CharacterClassEscape | CharacterEscape`, i.e.
`\\-` is EXPLICITLY legal inside a character class when the `u` flag is set --
and `-` is missing from that allow-list. Two of Turnstile's seven patterns are
`\\-` inside a class with flags `iu`. If the hypothesis holds, exactly those two
fail and the other five compile, INCLUDING the two that use `\\/` under `u` --
because `/` IS in the allow-list. That last part is what makes this a test
rather than a guess: a wrong hypothesis about the allow-list would take `\\/`
down with `\\-`, and a hypothesis that just said "unicode regexes are broken"
would predict all seven failing.

NOT IN THIS FILE'S GROUND. libregexp.c is third_party and another workflow is
editing quickjs.c right now. This measures and reports; it changes nothing.
Nothing here is tuned so that any checker accepts anything, and identifying why
a script fails to PARSE is not working around what the script does.
"""

import os
import re
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session        # noqa: E402

ISO, DISK = sys.argv[1], sys.argv[2]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# The seven, verbatim. Written as a JS array of [pattern, flags] pairs; the
# backslashes below are what the JS source literally contains.
CASES = r"""[
 ["^https:\\/\\/(?:challenges(?:\\.fed)?\\.cloudflare\\.com|challenges\\.cloudflare-cn\\.com)\\/turnstile\\/v0(?:\\/.*)?\\/api\\.js", "u"],
 ["\\/turnstile\\/v0(?:\\/.*)?\\/api\\.js", "u"],
 ["^[0-9A-Za-z_-]{3,100}$", "u"],
 ["^[a-z0-9_-]{0,32}$", "iu"],
 ["^[a-z0-9_\\-=]{0,255}$", "iu"],
 ["^[a-z]{2,3}(?:[-_][a-z]{2})?$", "iu"],
 ["^[0-9a-z_\\-.]{5,2000}$", "iu"]
]"""

# Minimal pairs, to separate "the u flag" from "the \- escape" from "inside a
# character class". Each differs from its neighbour in ONE way, which is the
# only shape that can attribute a failure to a cause.
PROBES = r"""[
 ["[a-z\\-=]",   "",   "backslash-hyphen in a class, NO u flag"],
 ["[a-z\\-=]",   "u",  "backslash-hyphen in a class, WITH u flag"],
 ["[a-z\\/=]",   "u",  "backslash-slash in a class, WITH u flag (control: / IS in the allow-list)"],
 ["a\\-b",       "",   "backslash-hyphen OUTSIDE a class, no u flag"],
 ["a\\-b",       "u",  "backslash-hyphen OUTSIDE a class, WITH u flag (spec says this one IS invalid)"],
 ["[a-z-=]",     "u",  "bare hyphen in a class, u flag (control: must compile)"],
 ["\\/x\\/",     "u",  "backslash-slash outside a class, u flag (control: must compile)"]
]"""

PAGE = """<style>body{background:#fff;margin:0}div{font-size:28px}</style>
<div id="after">REGEX</div>
<script>
function log(s) { try { console.log(s); } catch (e) {} }
var CASES = %s;
var PROBES = %s;
for (var i = 0; i < CASES.length; i++) {
  var p = CASES[i][0], f = CASES[i][1];
  try { new RegExp(p, f); log('RX-CASE ' + i + ' OK flags=' + f); }
  catch (e) { log('RX-CASE ' + i + ' THREW flags=' + f + ' ' + e.name + ': ' + e.message); }
}
for (var j = 0; j < PROBES.length; j++) {
  var q = PROBES[j];
  try { new RegExp(q[0], q[1]); log('RX-PROBE ' + j + ' OK | ' + q[2]); }
  catch (e) { log('RX-PROBE ' + j + ' THREW ' + e.name + ' | ' + q[2]); }
}
log('RX-DONE');
</script>""" % (CASES, PROBES)

tmp = tempfile.mkdtemp(prefix="qmp_rx_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        raw = ("<!doctype html><html><head><title>rx</title></head><body>" +
               PAGE + "</body></html>\n").encode()
        self.send_response(200)
        self.send_header("Content-Type", "text/html")
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


def serial():
    try:
        with open(serial_path, "rb") as fh:
            return fh.read().decode("utf-8", "replace")
    except OSError:
        return ""


def die(msg):
    print("FAIL: " + msg)
    print(serial()[-6000:])
    proc.kill()
    sys.exit(1)


def wait_serial(needle, secs, what):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial():
            return True
        if proc.poll() is not None:
            die("QEMU exited while waiting for " + what)
        time.sleep(0.25)
    return False


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

    ui.typ("http://10.0.2.2:%d/rx.html" % PORT)
    ui.key("ret")
    if not wait_serial("RX-DONE", 150, "the regex probe"):
        die("the regex probe never finished")

    print("\n=== Turnstile's seven RegExp() calls, compiled one at a time ===")
    for ln in re.findall(r"\bRX-CASE (.*)", serial()):
        print("   " + ln.strip())
    print("\n=== minimal pairs: which property is actually refused ===")
    for ln in re.findall(r"\bRX-PROBE (.*)", serial()):
        print("   " + ln.strip())
    proc.kill()
    sys.exit(0)
except SystemExit:
    raise
except Exception as exc:                    # noqa: BLE001
    import traceback
    traceback.print_exc()
    die("harness error: %r" % (exc,))
