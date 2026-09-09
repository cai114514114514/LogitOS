#!/usr/bin/env python3
"""qmp_zaiblank_page -- replay a saved BLANK-class specimen IN THE GUEST, with
its real captured subresources, and classify WHY it paints nothing.

    python3 tests/qmp/qmp_zaiblank_page.py --iso build-zaiblank/logit.iso \
        --disk build-zaiblank/disk.img \
        --fixture tests/fixtures/zaiblank/zai --name zai --out /tmp/zai.json

WHY THIS HARNESS EXISTS (and why qmp_replay.py was not enough)
==============================================================
qmp_replay.py replays ONE saved document and REFUSES every subresource -- the
right shape for "does this engine still throw X on these exact bytes", the
wrong shape for a chat-SPA death, because a modern SPA's <body> is empty in
the committed HTML and every pixel it will ever paint is created by a module
script the shell <link>s. Refusing the bundle measures the shell, not the
death. This driver serves every file in the fixture directory (captured
host-side with the browser's own honest User-Agent, see tests/fixtures/
zaiblank/PROVENANCE.md) by BASENAME under the replay origin, so a page whose
scripts are named with absolute CDN URLs still finds them: '//cdn.example/x.js'
is protocol-relative and resolves against the replay origin, and https://
URLs on other hosts go to the live network exactly as they would on a real
load (never intercepted -- intercepting them would need a proxy, and a
half-faked network is worse than an honest one).

NOTHING IS FABRICATED. Only files actually captured from the wire are served,
byte-for-byte; anything else 404s and is recorded in subresource_404. An SPA's
/api/* POSTs 404 by design -- the replay is offline, and a page that cannot
degrade without its API is a page whose death classification says so.

WHAT IS MEASURED (the zaiblank package's question: WHERE does a BLANK page die?)
  classification   FETCH-FAIL        the document itself never loaded
                   BLANK-before-JS   loaded, no exception, no module ran, and
                                     the body never grew -- the pipeline (HTML
                                     parse / CSS / layout), not the runtime
                   BLANK-after-JS-death   loaded, module(s) ran, FIRST
                                     exception recorded with its stack, body
                                     never grew past the shell
                   BLANK-silent      loaded, no exception anywhere, modules
                                     ran (or should have), body never grew --
                                     the "silent freeze" class: no signal on
                                     the wire, nothing painted
                   STRUCTURAL-REFUSAL  the first death line is an engine
                                     refusal (NotSupportedError / bare module
                                     specifier / module fetch FAILED on a
                                     specifier the fixture HAS -- i.e. a load
                                     defect dressed as a page defect)
                   MOUNTED           body grew and painted text runs appeared
  first_exception the first JS-relevant serial line after navigation, verbatim
  mount ladder     #ZAIB t<ms> body=<len> text=<chars> nodes=<n> snapshots at
                  t0/microtask/250/1000/2500/4000/7000 ms -- a ladder that
                  never leaves 0 is the blank, a ladder that climbs and stops
                  is a partial mount
  painted text     the engine's own [dl] painted text dump, via about:text,
                  confirmed-and-retried the way qmp_site.py learned to (a dump
                  that never arrives is recorded as missing, not as zero)

The reporter is injected server-side as a CLASSIC <script> before </body> --
never into the committed bytes -- for the reason guest_mount.py states: it
must observe the PRE-module document at t0, and it must survive a control
build in which module evaluation is compiled out.

Exit code is about the HARNESS, not the page (same rule as qmp_site.py): a
page that dies is a result, and the JSON is where the result lives.
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
from qmp_ui import Session       # noqa: E402

PARK = (40, 780)
BOOT_BUDGET = float(os.environ.get("ZAIBLANK_BOOT", 420))
SELFTEST_BUDGET = float(os.environ.get("ZAIBLANK_SELFTEST", 120))
LOAD_BUDGET = float(os.environ.get("ZAIBLANK_LOAD", 240))
SETTLE = float(os.environ.get("ZAIBLANK_SETTLE", 30))

SELFTEST = ("<!doctype html><html><head><title>sb</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('ZB-READY-%s');</script></body></html>")

# Generic, site-agnostic: any SPA that mounts grows <body>, its text content,
# and its node count. No hostname, no framework, no bundle name -- sites are
# specimens; the classifier answers in engine vocabulary only.
REPORTER = """(function () {
function snap(when) {
  try {
    var b = document.body;
    var nodes = 0;
    if (b && b.getElementsByTagName) nodes = b.getElementsByTagName('*').length;
    var txt = '';
    try { txt = b && b.innerText ? String(b.innerText) : ''; } catch (e) {}
    console.log('#ZAIB t' + when + ' body=' + (b && b.innerHTML ? b.innerHTML.length : 0)
      + ' text=' + txt.replace(/\\s+/g, ' ').trim().length + ' nodes=' + nodes);
  } catch (e) { console.log('#ZAIB t' + when + ' REPORT-THREW ' + e); }
}
snap(0);
Promise.resolve().then(function () { snap('mt'); });
[250, 1000, 2500, 4000, 7000].forEach(function (ms) {
  setTimeout(function () { snap(ms); }, ms);
});
})();
"""

# The ladder times are in ms and SETTLE is in SECONDS; the last snapshot
# fires at 7 s, so a settle budget below 10 s would read a page mid-ladder.
assert 7 + 3 <= SETTLE, "SETTLE must exceed the reporter's last snapshot"

JS_LINE_TAGS = ("[browser] JS exception:", "[js] module exception in",
                "[browser] module rejected", "[js] uncaught in event listener:",
                "[js] uncaught in timer:", "[js] bare module specifier",
                "[js] module fetch FAILED", "[browser] module exception")


def ctrl(ui, ch):
    ui.key_mods(["ctrl"], ch)
    time.sleep(0.3)


def classify(rec, ladder, exceptions, modules_loaded, modules_failed):
    """The BLANK-class taxonomy, in engine vocabulary. Order matters: a
    structural refusal is more specific than a generic JS death, and a page
    whose document never loaded has no JS story to tell at all."""
    if not rec.get("loaded"):
        return "FETCH-FAIL", "the document itself never finished loading"
    grew = ladder and max(ladder.values(), default=(0, 0, 0))[0] > 64
    painted = (rec.get("text_runs") or 0) > 8
    if grew and painted:
        return "MOUNTED", "body grew and the engine painted %s text run(s)" % rec.get("text_runs")
    first = exceptions[0] if exceptions else None
    if first:
        # An engine refusal naming a capability (not the page's own logic)
        # is the STRUCTURAL class the zaiblank brief asks to rank.
        if ("is not supported in this build" in first
                or "bare module specifier" in first
                or "NotSupportedError" in first):
            return "STRUCTURAL-REFUSAL", first
        if modules_loaded or grew:
            return "BLANK-after-JS-death", first
        return "BLANK-before-JS", first
    if modules_failed and not modules_loaded:
        return "STRUCTURAL-REFUSAL", ("module fetch failed for %d specifier(s) the "
                                      "fixture did serve or the loader mis-resolves"
                                      % modules_failed)
    if modules_loaded or grew:
        return "BLANK-silent", ("no exception on the wire, but the body never grew "
                                "past the shell (silent freeze)")
    return "BLANK-before-JS", "no exception and no module ever ran"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", default="build-zaiblank/logit.iso")
    ap.add_argument("--disk", default="build-zaiblank/disk.img")
    ap.add_argument("--fixture", required=True, help="directory with index.html + captured files")
    ap.add_argument("--name", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--serial", default=None)
    ap.add_argument("--shot", default=None, help="where to put the settled screendump PPM")
    args = ap.parse_args()

    fx = os.path.abspath(args.fixture)
    doc_path = os.path.join(fx, "index.html")
    if not os.path.exists(doc_path):
        print(json.dumps({"name": args.name, "verdict": "HARNESS",
                          "why": "no index.html in %s" % fx}))
        return 0
    with open(doc_path, "rb") as fh:
        doc = fh.read()

    # Server-side reporter injection: the committed specimen bytes are never
    # edited (frameworks' guest_mount.py precedent). Inserted before </body>
    # so it is later in document order than every app script -- the t0
    # snapshot then reads the PRE-module document.
    text = doc.decode("utf-8", "replace")
    if "</body>" in text:
        text = text.replace("</body>", "<script>%s</script></body>" % REPORTER, 1)
    else:
        text += "<script>%s</script>" % REPORTER
    doc_reported = text.encode("utf-8")

    files = {}
    for fn in os.listdir(fx):
        p = os.path.join(fx, fn)
        if os.path.isfile(p) and fn != "index.html" and not fn.startswith("."):
            with open(p, "rb") as fh:
                files[fn] = fh.read()

    token = "%d" % (os.getpid() & 0xFFFF)
    selftest = (SELFTEST % token).encode()
    misses = []

    CT = {"html": "text/html; charset=utf-8", "js": "text/javascript",
          "mjs": "text/javascript", "css": "text/css", "json": "application/json",
          "svg": "image/svg+xml", "png": "image/png", "ico": "image/x-icon",
          "woff2": "font/woff2", "woff": "font/woff", "map": "application/json"}

    class Serve(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def _send(self, body, code=200, ctype="text/html; charset=utf-8"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body)
            except OSError:
                pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path == "/sb.html":
                self._send(selftest)
            elif path in ("/", "/index.html"):
                self._send(doc_reported)
            else:
                base = path.rsplit("/", 1)[-1]
                if base in files:
                    ext = base.rsplit(".", 1)[-1].lower()
                    self._send(files[base], 200, CT.get(ext, "application/octet-stream"))
                else:
                    misses.append(self.path)
                    self._send(b"<!doctype html>not here", 404)

        def do_POST(self):
            misses.append("POST " + self.path)
            self._send(b"", 404, "application/json")

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    tmp = tempfile.mkdtemp(prefix="zaiblank_%s_" % re.sub(r"\W+", "_", args.name))
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")

    rec = {"name": args.name, "fixture": fx, "bytes": len(doc),
           "verdict": "HARNESS", "why": "did not run",
           "url": "http://10.0.2.2:%d/" % port,
           "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "exceptions": [], "subresource_404": [], "mount_ladder": {},
           "text_runs": None, "text_bytes": None, "serial_log": None}

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "1024M", "-smp", "4", "-accel", "tcg,thread=multi",
           "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
           "-display", "none", "-no-reboot",
           "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
           "-serial", "file:" + serial_path,
           "-qmp", "unix:%s,server,nowait" % qmp_path]

    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            with open(args.out, "w") as fh:
                json.dump(rec, fh, indent=1)
            print(json.dumps({"name": args.name, "verdict": "HARNESS", "why": rec["why"]}))
            return 0

    qlog = open(os.path.join(tmp, "qemu.log"), "wb")
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
        rec["subresource_404"] = misses
        dst = args.serial or os.path.join(os.path.dirname(os.path.abspath(args.out)),
                                          "%s.zaiblank.serial.txt" % args.name)
        try:
            with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(serial())
            rec["serial_log"] = dst
        except OSError:
            pass
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w") as fh:
            json.dump(rec, fh, indent=1, ensure_ascii=False)
        print(json.dumps(rec, indent=1, ensure_ascii=False))
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
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("ZB-READY-" + token, SELFTEST_BUDGET, mark):
            finish("HARNESS", "the self-test page never loaded -- nothing was "
                              "measured about the specimen")

        ctrl(ui, "t")
        time.sleep(1.5)
        ui.goto(*PARK)

        mark = len(serial())
        ctrl(ui, "l")
        ui.typ(rec["url"])
        ui.key("ret")
        if not wait_for("[browser] load: ", 20.0, mark):
            finish("HARNESS", "the address bar never took the URL")
        if not wait_for("[browser] load done", LOAD_BUDGET, mark):
            finish("FETCH-FAIL", "the page never finished loading")
        rec["loaded"] = True
        time.sleep(SETTLE)

        log = serial(mark)

        # The mount ladder: every #ZAIB line the reporter printed. The LAST
        # entry is the settled state -- same rule guest_mount.py applies.
        for m in re.finditer(r"#ZAIB t(\S+) body=(\d+) text=(\d+) nodes=(\d+)", log):
            rec["mount_ladder"][m.group(1)] = [int(m.group(2)), int(m.group(3)),
                                               int(m.group(4))]

        # First-exception chase: the FIRST JS-relevant line wins (the brief's
        # own rule -- read the first exception, not the cascade).
        seen = []
        for ln in log.splitlines():
            for tag in JS_LINE_TAGS + ("[error]",):
                i = ln.find(tag)
                if i >= 0:
                    msg = ln[i:].strip()
                    if msg not in seen:
                        seen.append(msg)
        rec["exceptions"] = seen

        rec["modules_loaded"], rec["modules_failed"] = 0, 0
        rec["modules_loaded"] = len(re.findall(r"\[js\] module loaded \d+ bytes", log))
        rec["modules_failed"] = len(re.findall(r"\[js\] module fetch FAILED", log))

        # Painted text runs: about:text prints the engine's own last-paint
        # accounting WITHOUT navigating. Typed-and-confirmed with retries --
        # the qmp_site.py lesson: a trigger that silently does not arrive is
        # recorded as missing, never as "painted nothing".
        runs = None
        nbytes = None
        for _ in range(3):
            frm = len(serial())
            ctrl(ui, "l")
            time.sleep(0.4)
            ui.typ("about:text")
            ui.key("ret")
            time.sleep(2.0)
            tail = serial(frm)
            if "[dl] painted text:" in tail:
                m = re.search(r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte", tail)
                if m:
                    runs, nbytes = int(m.group(1)), int(m.group(2))
                break
        rec["text_runs"] = runs
        rec["text_bytes"] = nbytes
        rec["text_dump_missing"] = runs is None

        if args.shot:
            try:
                ui.screendump(args.shot)
                rec["shot"] = args.shot
            except Exception as e:                      # noqa: BLE001
                rec["shot_error"] = repr(e)

        verdict, why = classify(rec, rec["mount_ladder"], seen,
                                rec["modules_loaded"], rec["modules_failed"])
        finish(verdict, why)
    except Exception as e:                                   # noqa: BLE001
        finish("HARNESS", "the harness itself raised: %r" % (e,))
    return 0


if __name__ == "__main__":
    sys.exit(main())
