#!/usr/bin/env python3
"""guest_mount -- the framework corpus measured IN THE GUEST, not by the probe.

    python3 tests/fixtures/frameworks/_guest/guest_mount.py <iso> <disk.img> \
        [--apps react,vite,...] [--json]

WHY THIS HARNESS EXISTS, and it is the qmp_currentscript.py lesson applied
before the fact rather than after it. tests/frameworks.mk measures the seven
framework builds with tests/unit/webapi_probe.c -- a HOST binary that links
every js_*.c TU but is its OWN EMBEDDER: it calls js_page_begin_script itself,
evaluates each script itself, and supplies its own bfetch. document.currentScript
spent months green in that probe while the shipped browser returned null on
every page, because the thing under test was the channel between browser.c and
the runtime and a host test IS a different embedder. A framework mount goes
through that same channel -- <script type=module> discovery in the HTML parser,
script fetch through browser_rt.c, module loading through js_module.c's loader,
the microtask/timer drain -- so the corpus's "6 of 7 run clean" is a hypothesis
until it is asked on the machine, with browser.c doing the calling.

WHAT EACH APP IS SERVED AS. One http.server PER APP, on its own loopback port,
each serving its app as the ONLY site at the root: "/" is the toolchain's own
index.html and every script is served at exactly the path the manifest says the
page names it. That shape is not convenience -- angular's document carries
<base href="/">, so serving it at /angular/ would resolve its relative
main-*.js against the root and the fixture would stop being the committed
bytes. Seven servers, one boot, one navigation each.

THE REPORTER is injected server-side (the committed fixtures are never edited):
a <script type="module"> appended before </body>, later in document order than
every app script, which reads the DOM back at t=0 (module body), the microtask
checkpoint, and 250/1000/2500/4000 ms. The LAST line a page prints is its
settled verdict -- same rule framework_rank.py applies to the host _paint run,
so the two instruments answer the same question.

WHAT IS COUNTED PER APP
  first-exception  the first JS-relevant line on the serial log after the
                   navigation: [browser] JS exception / [js] module exception /
                   [browser] module rejected / [error] / [js] uncaught in
                   timer / [js] bare module specifier / [js] module fetch
                   FAILED. Not the 404s of dropped CSS and favicons: the corpus
                   packing drops non-JS by design (pack.py says so), so a
                   favicon 404 is the fixture's shape, not a browser defect.
  paint            the settled #GUEST line (body length, mount lengths, the
                   button text, the lazy-route text)

Exit code is always 0 with no --min; with --min N it fails when fewer
than N apps settle NON-BLANK (body non-empty AND button text present), which
is the "≥N of 7 mounted" bar the wave-1 frameworks package is held to.
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

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "..", "..", "qmp"))
import qmp_addrbar  # noqa: E402
from qmp_ui import Session  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))          # .../frameworks/_guest
CORPUS = os.path.dirname(HERE)                             # .../frameworks
APPS = ["angular", "next", "react", "svelte", "vite", "vue", "webpack"]
QEMU = os.environ.get("QEMU", "qemu-system-x86_64")

# The settle ladder: the LAST snapshot printed is the verdict. 4000 ms is the
# top because angular mounts through a promise chain and next hydrates after
# its async scripts land -- both measured inside the settle window on this
# corpus; a page that needs longer than that is stalling, not settling.
LADDER = [250, 1000, 2500, 4000]

# INLINED AS A CLASSIC <script>, NOT served as a module, and that is a
# load-bearing choice twice over:
#   1. the t0 snapshot then reads the PRE-module document, which is what a
#      "before" should be; and the settled snapshots at 250..4000 ms do not
#      care when this body ran, only when each snap fires -- timers land after
#      every deferred module has evaluated;
#   2. tests/fragmk.mk's negative control silences js_module_eval entirely, and
#      a module-shaped reporter dies with it -- the first cut of this driver
#      did exactly that and the control read 0 of 7, indistinguishable from
#      "navigation broke". Classic-inline, the control reads the honest
#      partial number (webpack and next still mount: they ship classic
#      scripts), which is what separates "modules silenced" from "everything
#      silenced" -- the same argument the corpus's own README makes for why
#      per-cause tables beat per-framework ones.
REPORTER = """(function () {
var APP = %(app_json)s;
function snap(when) {
  try {
    var body = document.body;
    var html = body ? (body.innerHTML || "") : "";
    var mounts = ["root", "app", "app-root", "__next"];
    var found = "";
    for (var i = 0; i < mounts.length; i++) {
      var el = document.getElementById(mounts[i]);
      if (el) found += " #" + mounts[i] + "=" + (el.innerHTML || "").length;
    }
    var btn = document.getElementById("inc");
    var lazy = document.getElementById("lazy");
    console.log("#GUEST " + APP + " t" + when +
      " body=" + html.length + found +
      " button=" + (btn ? JSON.stringify(btn.textContent || "") : "none") +
      " lazy=" + (lazy ? JSON.stringify(lazy.textContent || "") : "none"));
  } catch (e) {
    console.log("#GUEST " + APP + " t" + when + " REPORT-THREW " + e);
  }
}
snap(0);
Promise.resolve().then(function () { snap(1); });
%(ladder)s.forEach(function (ms) {
  setTimeout(function () { snap(ms); }, ms);
});
})();
"""

# What counts as a JS exception line, on OUR serial log. Each prefix is a
# different reporter (js_page.c for classic, js_module.c for modules, the
# promise/timer paths), because which one fires first is itself diagnostic.
EXC_RES = [
    re.compile(r"^\[browser\] JS exception: (.*)"),
    re.compile(r"^\[js\] module exception in (\S+): (.*)"),
    re.compile(r"^\[browser\] module rejected (\S+): (.*)"),
    re.compile(r"^\[error\] (.*)"),
    re.compile(r"^\[js\] uncaught in timer: (.*)"),
    re.compile(r"^\[js\] bare module specifier (.*)"),
    re.compile(r"^\[js\] module fetch FAILED: (.*)"),
]
GUEST_RE = re.compile(r"^#GUEST (\S+) t(\S+) (.*)$")


def load_app(name):
    """(index.html with the reporter inlined, {url-path: bytes}) for `name`."""
    d = os.path.join(CORPUS, name)
    html = open(os.path.join(d, "index.html"), "rb").read()
    # Classic INLINE, before </body>: no fetch, no module machinery -- see the
    # comment on REPORTER for why a module-shaped reporter is the wrong shape.
    tag = ("<script>" + reporter_js(name) + "</script>").encode()
    if b"</body>" not in html:
        raise SystemExit("guest_mount: %s/index.html has no </body>" % name)
    html = html.replace(b"</body>", tag + b"</body>", 1)
    files = {"/": html}
    for line in open(os.path.join(d, "manifest.txt"), encoding="utf-8"):
        line = line.rstrip("\n")
        if not line or "\t" not in line:
            continue
        key, local = line.split("\t", 1)
        if key.startswith("/"):
            url = key                     # absolute as the page names it
        elif key.startswith("http"):
            continue                      # full URL: another origin, not ours
        else:
            url = "/" + key               # the document sits at the site root
        p = os.path.normpath(os.path.join(d, local))
        if url not in files and os.path.exists(p):
            files[url] = open(p, "rb").read()
    return html, files


def reporter_js(name):
    return REPORTER % {"app_json": json.dumps(name),
                       "ladder": json.dumps(LADDER)}


def start_server(name, log):
    """One ThreadingHTTPServer per app, serving it as the only site at '/'."""
    html, files = load_app(name)

    class H(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            path = self.path.split("?")[0]
            log.append((name, self.path))
            body = files.get(path)
            ctype = "text/html" if path == "/" else (
                "text/css" if path.endswith(".css") else
                "image/x-icon" if path.endswith(".ico") else "text/javascript")
            raw = body if body is not None else b"not found\n"
            self.send_response(200 if body is not None else 404)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_port


def first_exception(chunk):
    """The first JS-relevant line, or None. `chunk` is serial since navigate."""
    e = all_exceptions(chunk)
    return e[0] if e else None


def all_exceptions(chunk):
    """Every JS-relevant line, in order. The FIRST is the work order; the rest
    say whether the page degraded after it or soldiered on -- angular mounts
    fully and still logs two lines, and a driver that printed only the first
    would leave that unexplained."""
    out = []
    for ln in chunk.splitlines():
        s = ln.strip()
        for rx in EXC_RES:
            if rx.match(s):
                out.append(s)
                break
    return out


def last_paint(chunk, name):
    """The settled #GUEST line for `name`: highest t, latest printed."""
    last = None
    for ln in chunk.splitlines():
        m = GUEST_RE.match(ln.strip())
        if m and m.group(1) == name:
            last = (m.group(2), m.group(3))
    return last


def nonblank(paint):
    """body=<n> with n>0 AND a real button text -- the mount bar."""
    if not paint:
        return False
    m = re.search(r"body=(\d+)", paint[1])
    btn = re.search(r'button="?(.*?)"?\s+lazy=', paint[1])
    return bool(m and int(m.group(1)) > 0 and btn and btn.group(1) not in ("", "none"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("iso")
    ap.add_argument("disk")
    ap.add_argument("--apps", default=",".join(APPS))
    ap.add_argument("--min", dest="min_mounts", type=int, default=0,
                    metavar="N", help="fail unless at least N apps settle non-blank")
    ap.add_argument("--boot-timeout", type=int, default=240)
    ap.add_argument("--settle", type=int, default=75,
                    help="seconds to wait per app for its last #GUEST line")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args()
    apps = [a for a in args.apps.split(",") if a]

    requested = []
    servers = {}
    for name in apps:
        srv, port = start_server(name, requested)
        servers[name] = "http://10.0.2.2:%d/" % port

    tmp = tempfile.mkdtemp(prefix="qmp_frmw_")
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")
    proc = subprocess.Popen(
        [QEMU, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
         "-drive", "file=%s,format=raw,if=none,id=hd0" % args.disk,
         "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
         "-snapshot", "-m", "1024M", "-smp", "4", "-accel", "tcg,thread=multi",
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
        print("----- paths the fixture servers were asked for (last 60) -----")
        for name, p in requested[-60:]:
            print("  %s %s" % (name, p))
        print("----- serial (tail) -----")
        print(serial()[-6000:])
        print("-------------------------")
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
        if not wait_serial("LOGIT_BOOT_OK", args.boot_timeout, "boot"):
            die("kernel never printed LOGIT_BOOT_OK")
        if not wait_serial("desktop live", 90, "desktop"):
            die("the window manager never brought the desktop up")
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            die(str(e))
        time.sleep(6)

        results = {}
        bar = None
        for name in apps:
            mark = ui.mark()          # serial offset: everything before is
                                      # another app's page
            # Navigate through the address bar, the same real channel every
            # other page driver here uses. qmp_addrbar.focus() is LOAD-BEARING
            # from the SECOND navigation on: the browser boots with the bar in
            # edit mode (why the first typing landed without a click), and
            # after an Enter it is not editing again. The first guest run of
            # this driver typed blind and mounted only the FIRST app in the
            # list -- six silent no-op navigations, which is exactly the
            # failure shape qmp_addrbar's header documents.
            bar = qmp_addrbar.focus(ui, origin=bar)
            for _ in range(90):
                ui.key("backspace", settle=0.02)
            ui.typ(servers[name])
            ui.key("ret")

            # Wait for the page's own scripts to have fetched (any module-load
            # line naming this server) before starting the settle clock.
            deadline = time.time() + args.settle
            paint = None
            while time.time() < deadline:
                chunk = ui.serial_text()[mark:]
                paint = last_paint(chunk, name)
                if paint and paint[0] == str(LADDER[-1]):
                    break
                time.sleep(1.0)
            chunk = ui.serial_text()[mark:]
            paint = last_paint(chunk, name)
            results[name] = {
                "first_exception": first_exception(chunk),
                "all_exceptions": all_exceptions(chunk),
                "paint_t": paint[0] if paint else None,
                "paint": paint[1] if paint else None,
                "nonblank": nonblank(paint),
            }
            time.sleep(1.0)
    finally:
        try:
            proc.kill()
        except OSError:
            pass

    n_blank = sum(1 for r in results.values() if not r["nonblank"])
    print()
    print("== framework corpus IN THE GUEST: first exception + settled DOM ==")
    for name in apps:
        r = results[name]
        exc = r["first_exception"] if r["first_exception"] else "(none)"
        print("%-9s %-4s exc: %s" % (name, "MOUNT" if r["nonblank"] else "----", exc))
        for extra in r["all_exceptions"][1:6]:
            print("%-9s      also: %s" % ("", extra))
        print("%-9s      paint[t%s]: %s" % ("", r["paint_t"] or "?", r["paint"] or "(no #GUEST line)"))
        got = [p for n, p in requested if n == name]
        print("%-9s      server saw %d request(s): %s"
              % ("", len(got), " ".join(sorted(set(got))[:6]) or "NONE -- the browser never fetched the app"))
    print("-- %d of %d settled non-blank" % (len(apps) - n_blank, len(apps)))

    if args.json:
        print("#JSON " + json.dumps(results))

    if args.min_mounts and (len(apps) - n_blank) < args.min_mounts:
        print("guest_mount: FAIL -- %d of %d non-blank, the bar is %d"
              % (len(apps) - n_blank, len(apps), args.min_mounts))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
