#!/usr/bin/env python3
"""Prove, ON THE REAL MACHINE, that a page fetches a .wasm, instantiates it and
gets the right answers back.

    python3 tests/qmp/qmp_wasm_page.py <iso> <disk.img> [--negctl]

WHY THIS EXISTS WHEN THERE IS ALREADY A HOST GATE.  tests/unit/wasm_js_test.c
runs 64 checks against the same API and passes; it links QuickJS and js_wasm.c
into a host binary and drives them directly.  That proves the arithmetic and
nothing about the browser.  Between the two sits everything this file is for:
the .aex actually containing js_wasm.o, js_page.c actually calling
js_wasm_install, the WebAssembly global actually surviving into a page's script
context, fetch() actually delivering the bytes, and Response.arrayBuffer()
actually being the thing instantiateStreaming falls back to.  CLAUDE.md's
sharpest scar is exactly this gap -- `make test-wpt ONLY=css/css-grid` read
531/11152 with and without the grid implementation, because linking a
translation unit is not running it.  "The API exists" is that shape.  This is
the gate that is not.

THE NUMBERS ARE CHOSEN BY THE HARNESS, FRESH ON EVERY RUN, and that is the
whole design rather than a flourish.  A page that printed `WASM-ADD 42` would
satisfy a naive matcher whether or not a single wasm instruction executed --
and a hand-written expectation only records what its author already believed.
So the server bakes three random values into the page and the harness asserts
the results it computed itself:

    ADD      a + b            an exported i32 function, called with our numbers
    IMPORT   4 * b            a JS closure the harness wrote, CALLED FROM wasm
    I64      a*10^9 + 1       a value no double can hold, as a BigInt
    MEMPEEK  magic            wasm read back what wasm stored
    MEMVIEW  magic            and the page read the SAME bytes through an
                              ArrayBuffer aliasing the module's linear memory
    DETACH   0                a grow from INSIDE wasm detached the page's view

A canned page cannot produce those.  Neither can a WebAssembly object that
merely exists.

THE NEGATIVE CONTROL (--negctl) SERVES A TRUNCATED add.wasm and nothing else
changes: same page, same numbers, same everything.  It must report a
CompileError and must NOT report an ADD line.  Without it, "the page printed
the right sum" is equally consistent with a browser that accepts any bytes at
all and with one that validates -- and this whole line of work is a validator.
A control that cannot be watched failing is worse than no control.

THE MODULE BYTES COME FROM tests/unit/wasm_js_modules.inc, parsed out of the C
array literals rather than assembled here.  One jar, one door: those bytes are
already the host gate's corpus and are already checked against their .wat by
tools/wasm_js_modules.py --check.  A second copy in this file would be a second
door onto the same jar, and CLAUDE.md counts three times this tree has paid for
exactly that.
"""

import os
import random
import re
import subprocess
import sys
import tempfile
import threading
import time
import http.server

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM      # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
ISO, DISK = sys.argv[1], sys.argv[2]
NEGCTL = "--negctl" in sys.argv[3:]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

INC = os.path.join(ROOT, "tests", "unit", "wasm_js_modules.inc")


def load_modules():
    """Recover the assembled module bytes from the C header the host gate uses.

    Deliberately strict: a name that is missing, or an array that does not
    begin with the wasm magic, is a hard error here rather than a mysterious
    CompileError inside the guest twenty minutes later.  The apparatus should
    fail where the apparatus is wrong."""
    try:
        text = open(INC).read()
    except OSError:
        sys.exit("qmp_wasm_page: %s is absent -- run "
                 "tools/wasm_js_modules.py --write" % INC)
    out = {}
    for m in re.finditer(r"static const unsigned char W_(\w+)\[\] = \{(.*?)\};",
                         text, re.S):
        name, body = m.group(1), m.group(2)
        out[name] = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]{2})", body))
    for need in ("add", "mem", "i64", "imp"):
        if need not in out:
            sys.exit("qmp_wasm_page: W_%s missing from %s" % (need, INC))
        if out[need][:4] != b"\0asm":
            sys.exit("qmp_wasm_page: W_%s does not start with the wasm magic" % need)
    return out


MODS = load_modules()

rnd = random.Random()
A = rnd.randrange(1000, 90000)
B = rnd.randrange(3, 900)
MAGIC = rnd.randrange(1, 0x7FFFFFFF)

EXPECT = {
    "ADD": str(A + B),
    "IMPORT": str(4 * B),
    "I64": str(A * 10**9 + 1),
    "MEMPEEK": str(MAGIC),
    "MEMVIEW": str(MAGIC),
    "DETACH": "0",
}

PAGE = """<!doctype html><html><head><title>wasm</title></head>
<body style="background:#ffffff">
<div id="out" style="font-size:22px">wasm: running</div>
<script>
var A = %(A)d, B = %(B)d, MAGIC = %(MAGIC)d;
function say(s) { console.log('WASMGUEST ' + s); }
function bail(tag, e) {
  say(tag + '-THREW ' + ((e && (e.name + ': ' + e.message)) || String(e)));
}
say('TYPEOF ' + (typeof WebAssembly));
if (typeof WebAssembly !== 'undefined') {
  say('HAS ' + ['Module','Instance','Memory','Table','Global','validate',
                'compile','instantiate'].filter(function (k) {
        return typeof WebAssembly[k] !== 'undefined'; }).join(','));
  say('STREAMING ' + (typeof WebAssembly.instantiateStreaming));
}
(async function () {
  var ok = 0;
  /* 1. fetch -> arrayBuffer -> instantiate -> call, with the harness's numbers */
  try {
    var b1 = await (await fetch('add.wasm')).arrayBuffer();
    var r1 = await WebAssembly.instantiate(b1, {});
    say('ADD ' + r1.instance.exports.add(A, B));
    ok++;
  } catch (e) { bail('ADD', e); }

  /* 2. instantiateStreaming.  Response.body is undefined in this browser, so
        this falls back to arrayBuffer() -- conformant, and the point here is
        that the ENTRY POINT works, not that bytes arrived incrementally. */
  try {
    var r2 = await WebAssembly.instantiateStreaming(fetch('mem.wasm'), {});
    var ex = r2.instance.exports;
    ex.poke(0, MAGIC);
    say('MEMPEEK ' + ex.peek(0));
    say('MEMVIEW ' + new DataView(ex.mem.buffer).getUint32(0, true));
    /* a grow from INSIDE wasm must detach the view the page already holds */
    var before = ex.mem.buffer;
    ex.growit(1);
    say('DETACH ' + before.byteLength);
    say('REGROWN ' + ex.mem.buffer.byteLength);
    ok++;
  } catch (e) { bail('MEM', e); }

  /* 3. i64 across the boundary, as a BigInt, at a magnitude a double loses */
  try {
    var b3 = await (await fetch('i64.wasm')).arrayBuffer();
    var r3 = await WebAssembly.instantiate(b3, {});
    say('I64 ' + r3.instance.exports.inc(BigInt(A) * 1000000000n).toString());
    ok++;
  } catch (e) { bail('I64', e); }

  /* 4. a JavaScript closure called FROM wasm */
  try {
    var b4 = await (await fetch('imp.wasm')).arrayBuffer();
    var r4 = await WebAssembly.instantiate(b4, { env: { twice: function (x) {
      return x * B; } } });
    say('IMPORT ' + r4.instance.exports.call4());
    ok++;
  } catch (e) { bail('IMPORT', e); }

  /* 5. the validator is real: bytes with the magic and nothing else must be a
        CompileError, not an instance. */
  try {
    new WebAssembly.Module(new Uint8Array([0,0x61,0x73,0x6d,1,0,0,0,9,9,9,9]));
    say('BADMOD ACCEPTED');
  } catch (e) { say('BADMOD ' + e.name); }

  document.getElementById('out').textContent = 'wasm: ' + ok + '/4';
  say('DONE ' + ok);
})();
</script></body></html>
""" % {"A": A, "B": B, "MAGIC": MAGIC}

tmp = tempfile.mkdtemp(prefix="wasmpage_")
qmp_path = os.path.join(tmp, "qmp.sock")
serial_path = os.path.join(tmp, "serial.log")

requested = []

# The one difference the control makes.  Everything else -- the page, the
# numbers, the other three modules -- is identical, so a difference in the
# result can only come from the bytes.
ADD_BYTES = MODS["add"][: len(MODS["add"]) // 2] if NEGCTL else MODS["add"]

FILES = {
    "/add.wasm": ADD_BYTES,
    "/mem.wasm": MODS["mem"],
    "/i64.wasm": MODS["i64"],
    "/imp.wasm": MODS["imp"],
}


class Fixture(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def do_GET(self):
        path = self.path.split("?")[0]
        requested.append(path)
        if path in ("/wasm.html", "/"):
            body, ctype = PAGE.encode(), "text/html"
        elif path in FILES:
            # THE CORRECT MIME TYPE, and it matters for one of the assertions:
            # instantiateStreaming is specified to reject anything that is not
            # application/wasm.  Serving octet-stream here would test our
            # fallback against a request no real deployment makes.
            body, ctype = FILES[path], "application/wasm"
        else:
            body, ctype = b"not found\n", "text/plain"
        self.send_response(200 if body != b"not found\n" else 404)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        try:
            self.wfile.write(body)
        except OSError:
            pass

    def log_message(self, *_a):
        pass


srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Fixture)
PORT = srv.server_port
threading.Thread(target=srv.serve_forever, daemon=True).start()

proc = subprocess.Popen(
    [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", ISO,
     "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % DISK,
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
    print("----- the harness chose: A=%d B=%d MAGIC=%d -----" % (A, B, MAGIC))
    print("----- expected -----")
    for k, v in EXPECT.items():
        print("  %-8s %s" % (k, v))
    print("----- paths the fixture server was asked for -----")
    print("\n".join(requested[-30:]) or "(none -- the guest never fetched anything)")
    print("----- WASMGUEST lines -----")
    print("\n".join(l for l in serial().splitlines() if "WASMGUEST" in l)
          or "(none -- the page's script never reported)")
    print("----- serial (tail) -----")
    print(serial()[-4000:])
    print("----- artefacts in %s -----" % tmp)
    try:
        proc.kill()
    except OSError:
        pass
    srv.shutdown()
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


def guest(tag):
    """The value the page reported for `tag`, or None."""
    for ln in serial().splitlines():
        i = ln.find("WASMGUEST " + tag + " ")
        if i >= 0:
            return ln[i + len("WASMGUEST " + tag + " "):].strip()
    return None


def goto(ui, url, bar=None):
    if bar:
        ui.click_at(bar[0], bar[1])
    for _ in range(90):
        ui.key("backspace", settle=0.02)
    ui.typ(url)
    ui.key("ret")


try:
    if not wait_serial("LOGIT_BOOT_OK", 300, "boot"):
        die("kernel never printed LOGIT_BOOT_OK")
    if not wait_serial("desktop live", 120, "desktop"):
        die("the window manager never brought the desktop up")
    time.sleep(3)

# One click, at the tile the GUEST names for browser.aex, verified against the
        # guest's own [wm] launched line -- the re-click loop this replaces was
        # the apology a hand-kept slot constant needed. See qmp_ui's dock block.
    ui = Session(qmp_path, serial=serial_path)
    try:
        ui.launch_app("browser")
    except AssertionError as e:
        die(str(e))
    time.sleep(7)

    base = "http://10.0.2.2:%d" % PORT
    goto(ui, base + "/wasm.html")

    ck(wait_serial("WASMGUEST TYPEOF", 120, "the page's script"),
       "the page loaded and its script ran in the real browser")

    # THE FEATURE TEST FIRST.  If this is 'undefined' every assertion below is
    # about a browser that has no WebAssembly, and saying so plainly is more
    # useful than six failures that all mean the same thing.
    ck(guest("TYPEOF") == "object",
       "typeof WebAssembly === 'object' in a real page (got %r)"
       % guest("TYPEOF"))
    ck(wait_serial("WASMGUEST DONE", 180, "the page's async work"),
       "the page's asynchronous work finished")

    ck("/wasm.html" in requested, "the guest fetched the page over the network")
    for f in ("/add.wasm", "/mem.wasm", "/i64.wasm", "/imp.wasm"):
        ck(f in requested, "the guest fetched %s as a subresource" % f)

    have = (guest("HAS") or "").split(",")
    for k in ("Module", "Instance", "Memory", "Table", "Global",
              "validate", "compile", "instantiate"):
        ck(k in have, "WebAssembly.%s is present in the page" % k)
    ck(guest("STREAMING") == "function",
       "instantiateStreaming is defined (it falls back to arrayBuffer here, "
       "because Response.body is undefined -- conformant, and stated as such)")

    if NEGCTL:
        # The control.  A truncated module must be REFUSED, and the refusal
        # must be the specified class rather than a generic failure.
        ck(guest("ADD") is None,
           "NEGCTL: the truncated add.wasm produced NO answer")
        t = guest("ADD-THREW") or ""
        ck(t.startswith("CompileError"),
           "NEGCTL: the truncated add.wasm was refused with a CompileError "
           "(got %r)" % t)
        # ... and everything else still worked, which is what makes the control
        # a control rather than a broken run: only add.wasm differed.
        ck(guest("I64") == EXPECT["I64"],
           "NEGCTL: the UNDAMAGED modules still gave the right answers, so the "
           "refusal above is about the bytes and not about the run")
        print("\nwasm-page NEGCTL: the browser refused a truncated module and "
              "kept working (A=%d B=%d)" % (A, B))
    else:
        for tag in ("ADD", "IMPORT", "I64", "MEMPEEK", "MEMVIEW", "DETACH"):
            got = guest(tag)
            ck(got == EXPECT[tag],
               "%s == %s (the harness chose it this run; got %r)"
               % (tag, EXPECT[tag], got))
        ck(guest("REGROWN") == "131072",
           "the memory really is 2 pages after the module grew itself "
           "(got %r)" % guest("REGROWN"))
        ck(guest("BADMOD") == "CompileError",
           "a module that is only a header is refused with a CompileError "
           "(got %r)" % guest("BADMOD"))
        ck(guest("DONE") == "4", "all four modules ran (got %r)" % guest("DONE"))

        # And the pixels, because the serial log alone cannot say the page is
        # still a page.  The script writes its tally into the document; if the
        # DOM write never landed the browser ran wasm and lost the result.
        time.sleep(2.0)
        p = PPM(ui.screendump(os.path.join(tmp, "page.ppm")))
        ck(p.dark_pixels((0, 0, p.w, p.h)) > 200,
           "the page is still painting text after all of that")
        print("\nwasm-page: a real page fetched four .wasm modules over HTTP, "
              "instantiated them in the browser and returned %d+%d=%d, "
              "4*%d=%d, %d*10^9+1 and magic %d -- every number chosen by this "
              "harness on this run"
              % (A, B, A + B, B, 4 * B, A, MAGIC))

    print("\nwasm-page: %d checks, all ok" % len(checks))
    proc.kill()
    srv.shutdown()
    sys.exit(0)

except SystemExit:
    raise
except Exception as e:                                    # noqa: BLE001
    die("harness error: %r" % (e,))
