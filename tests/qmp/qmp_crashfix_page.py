#!/usr/bin/env python3
"""The crash-cluster guest driver: one boot, one gate, four specimen replays.

    python3 tests/qmp/qmp_crashfix_page.py --iso ISO --disk DISK --out out.json \
        [--skip-fixture] [--only fixture,google-search,douyin,stripe,kimi-fm]

WHAT IS GATED AND WHAT IS EVIDENCE
===================================
The GATE is tests/fixtures/crashfix/inserted-datablock.html: a synthetic page
(the general defect, no site) that inserts data-block <script>s exactly the way
real loaders do and requires that they not execute while executable inserted
scripts still do. Its assertions decide the exit code.

The SPECIMENS are evidence, not gates -- an offline replay's origin, cookies
and URL-derived state all differ from the live load (see qmp_replay.py's own
header, whose argument this is), so a specimen can answer "does this engine
still throw X on these exact bytes" and never "does this site work". They are
recorded to the output JSON for the before/after tables in the report.

SPECIMEN SERVING, AND THE ONE PLACE THIS DELIBERATELY DIFFERS FROM qmp_replay
=============================================================================
qmp_replay.py refuses every subresource (404, recorded) because its specimens
are self-contained documents. Two of these four are not:

  google-search  document only; its five inline scripts are the specimen.
  douyin         the document is the specimen, but the crash it records lives
                 in a runtime-injected subresource the page's own obfuscated
                 code builds at run time (no absolute URL appears in the
                 document text, verified). Those requests are let through to
                 the REAL network -- the guest reaches it through SLIRP the
                 same way the site scoreboard does -- and every request the
                 harness served itself vs let through is recorded, because a
                 replay that silently mixes two sources is worse than one
                 that says so.
  stripe         fully offline: every https:// URL in the document text is
                 rewritten to this harness (script bodies come from the
                 specimen fetch recorded in PROVENANCE), so the run is
                 byte-stable.
  kimi-fm        a two-line loader page plus the saved fm.js: the whole point
                 is one subresource, and 111 of kimi's other 112 requests are
                 irrelevant to the crash being reproduced.

WHAT IS ASSERTED ABOUT THE FIXTURE PAGE
=======================================
  CRASHFIX-PAGE-LOADED          present   (the page's own script finished)
  "[browser] JS exception:"     ZERO      (the data blocks executed nothing)
  "skipping inserted <script"   >= 3      (the type gate fired, out loud)
  CRASHFIX-EXE-RAN              present   (an inserted script with NO type
                                           still runs -- the gate must not
                                           over-block; absence would be the
                                           "absent beats present-and-wrong"
                                           rule violated in the other dir)
  CRASHFIX-JSTYPE-RAN           present   (explicit text/javascript runs)
  CRASHFIX-INNERHTML-RAN        ABSENT    (innerHTML scripts never execute;
                                           already correct, asserted so it
                                           stays correct)
The module marker is REPORTED, not asserted -- see the fixture's own comment.
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
from qmp_ui import Session       # noqa: E402  the dock slot lives there, not here

PARK = (40, 780)
BOOT_BUDGET = float(os.environ.get("CRASHFIX_BOOT", 300))
SELFTEST_BUDGET = float(os.environ.get("CRASHFIX_SELFTEST", 90))
LOAD_BUDGET = float(os.environ.get("CRASHFIX_LOAD", 120))
SETTLE = float(os.environ.get("CRASHFIX_SETTLE", 12))

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
FIXTURE_DIR = os.path.join(ROOT, "tests", "fixtures", "crashfix")
SCOREBOARD = os.path.join(ROOT, "tests", "scoreboard", "full-corpus")

SELFTEST = ("<!doctype html><html><head><title>cf</title></head>"
            "<body style='background:#ffffff'><div style='font-size:28px'>READY</div>"
            "<script>console.log('CF-READY-%s');</script></body></html>")

EXC_TAGS = ("[browser] JS exception:", "[js] uncaught in event listener:",
            "[js] uncaught in timer:", "[error] Uncaught (in promise)")


def ctrl(ui, ch):
    ui.key_mods(["ctrl"], ch)
    time.sleep(0.3)


def specimen_pages(port):
    """name -> (doc_bytes, rewrite_absolute_urls, serve_map, note).

    rewrite: every 'https://<host>/' in the document becomes
    'http://10.0.2.2:<port>/<host>/' -- the server owns the whole path space,
    so what it does not have it 404s (and records). douyin's document contains
    no absolute URL at all; its injected subresources are built at run time
    and go to the real network on purpose (see the module docstring).
    """
    pages = {}
    with open(os.path.join(SCOREBOARD, "google-search.host.html"), "rb") as fh:
        pages["google-search"] = [fh.read(), True, {}, "scoreboard specimen, doc only"]
    with open(os.path.join(SCOREBOARD, "douyin.host.html"), "rb") as fh:
        pages["douyin"] = [fh.read(), True, {}, "scoreboard specimen; subresources LIVE"]
    with open(os.path.join(SCOREBOARD, "stripe.host.html"), "rb") as fh:
        pages["stripe"] = [fh.read(), True, {}, "scoreboard specimen; scripts local"]
    # secsdk-shape: the douyin crash reduced to its exact code shape. The real
    # specimen's crash lives in a runtime-injected subresource whose URL the
    # page's obfuscator builds at run time, so it cannot be served offline;
    # the FIRST STATEMENT of the crashing function (runtime_bundler_34.js,
    # byte 38277: document.currentScript.getAttribute("project-id")) is the
    # whole repro. Before the js_page_eval(node) fix this threw "cannot read
    # property 'getAttribute' of null" -- currentScript was null for every
    # dynamically inserted script -- and took the whole security SDK with it.
    pages["secsdk-shape"] = [
        ("<!doctype html><html><body><script>"
         "var s=document.createElement('script');s.src='/secsdk.js';"
         "document.body.appendChild(s);</script></body></html>").encode(),
        False,
        {"secsdk.js": ("secsdk-shape.js", "application/javascript")},
        "douyin crash reduced: currentScript.getAttribute in an inserted src script"]
    # kimi: the crash is one subresource. A loader page with the config the
    # real page sets before inserting it (measured in kimi's s010.js).
    kimi_loader = (
        "<!doctype html><html><head><title>fm</title></head><body>"
        "<script>window._fmOpt = {partner:'kimi', channel:'kimi-web', "
        "appName:'kimi-web', success:function(){}, "
        "getinfo:function(){return {rid:'t'};}};</script>"
        "<script>var s=document.createElement('script');s.src='/fm.js';"
        "document.head.appendChild(s);</script>"
        "</body></html>").encode()
    try:
        with open(os.path.join(FIXTURE_DIR, "trustdecision-fm.js"), "rb") as fh:
            pages["kimi-fm"] = [kimi_loader, False,
                                {"fm.js": ("trustdecision-fm.js", "application/javascript")},
                                "loader + saved fm.js"]
    except OSError:
        pages["kimi-fm"] = [kimi_loader, False, {},
                            "fm.js SPECIMEN MISSING from tests/fixtures/crashfix"]
    return pages


def rewrite_abs(doc, port):
    r = re.compile(rb"https://([A-Za-z0-9.\-]+)/")
    return r.sub(rb"http://10.0.2.2:%d/\\1/" % port, doc)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--disk", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--skip-fixture", action="store_true")
    ap.add_argument("--expect-broken", action="store_true",
                    help="NEGATIVE CONTROL: run against a browser built with "
                         "-DCRASHFIX_DATABLOCK_NOTOLD (the type gate switched "
                         "off = the defect itself) and require the fixture to "
                         "FAIL for exactly the right reasons: the data-block "
                         "syntax errors present, the skipping lines absent, "
                         "and the page otherwise alive. If this mode ever "
                         "passes the positive assertions, the harness is "
                         "measuring something other than the fix.")
    ap.add_argument("--only", default="",
                    help="comma list restricting which specimens run")
    args = ap.parse_args()

    only = [s.strip() for s in args.only.split(",") if s.strip()] if args.only else None
    token = "%d" % (os.getpid() & 0xFFFF)
    selftest = (SELFTEST % token).encode()
    served_local, served_live, misses = [], [], []
    pages = specimen_pages(0)          # doc bytes before rewrite; port filled later

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
            elif path == "/fx/inserted-datablock.html":
                with open(os.path.join(FIXTURE_DIR, "inserted-datablock.html"), "rb") as fh:
                    self._send(fh.read())
            elif path == "/page.html":
                self._send(main_page_doc[0])
            else:
                name = main_page_name
                smap = main_page_doc[2]
                hit = None
                for key, (fname, ctype) in smap.items():
                    if path == "/" + key:
                        hit = (fname, ctype)
                        break
                if hit:
                    try:
                        with open(os.path.join(FIXTURE_DIR, hit[0]), "rb") as fh:
                            served_local.append((name, path))
                            self._send(fh.read(), 200, hit[1])
                            return
                    except OSError:
                        pass
                misses.append((name, path))
                self._send(b"<!doctype html>not here", 404)

        do_POST = do_GET

        def log_message(self, *_a):
            pass

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Serve)
    port = srv.server_port
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    # fill the port-dependent pieces now that it exists
    for k, v in pages.items():
        v[0] = rewrite_abs(v[0], port) if v[1] else v[0]

    tmp = tempfile.mkdtemp(prefix="qmp_crashfix_")
    qmp_path = os.path.join(tmp, "qmp.sock")
    serial_path = os.path.join(tmp, "serial.log")

    rec = {"iso": os.path.abspath(args.iso), "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "fixture": None, "specimens": {}, "served_local": None, "served_live": None,
           "subresource_404": None, "verdict": "HARNESS", "why": "did not run"}

    main_page_doc = [pages["google-search"][0], True, {}]
    main_page_name = "google-search"

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    cmd = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-cdrom", args.iso,
           "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
           "-device", "virtio-blk-pci,drive=hd0", "-boot", "d",
           "-snapshot", "-m", "512M", "-smp", "4", "-accel", "tcg,thread=multi",
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
            print(json.dumps({"verdict": "HARNESS", "why": rec["why"]}))
            return 2

    proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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

    def finish(verdict, why, code):
        rec["verdict"] = verdict
        rec["why"] = why
        rec["finished"] = time.strftime("%Y-%m-%dT%H:%M:%S")
        rec["served_local"] = served_local
        rec["subresource_404"] = misses
        dst = os.path.join(os.path.dirname(os.path.abspath(args.out)), "crashfix.serial.txt")
        try:
            with open(dst, "w", encoding="utf-8", errors="replace") as fh:
                fh.write(serial())
        except OSError:
            pass
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        with open(args.out, "w") as fh:
            json.dump(rec, fh, indent=1, ensure_ascii=False)
        print(json.dumps({"verdict": verdict, "why": why, "fixture": rec["fixture"],
                          "specimens": {k: v["exceptions"] for k, v in rec["specimens"].items()}},
                         indent=1, ensure_ascii=False))
        sys.exit(code)

    def load_page(url, settle=SETTLE):
        """New tab -> address bar -> URL -> Enter. Returns the serial mark
        taken just before navigation, so the caller can read only this page's
        segment."""
        ctrl(ui, "t")
        time.sleep(1.0)
        ui.goto(*PARK)
        mark = len(serial())
        ctrl(ui, "l")
        ui.typ(url)
        ui.key("ret")
        if not wait_for("[browser] load: ", 25.0, mark):
            return mark, False
        wait_for("[browser] load done", LOAD_BUDGET, mark)
        time.sleep(settle)
        return mark, True

    def exceptions_in(seg):
        seen = []
        for ln in seg.splitlines():
            for tag in EXC_TAGS:
                i = ln.find(tag)
                if i >= 0:
                    msg = ln[i:].strip()
                    if msg not in seen:
                        seen.append(msg)
        return seen

    try:
        if not wait_for("LOGIT_BOOT_OK", BOOT_BUDGET):
            finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK", 2)
        if not wait_for("desktop live", 120):
            finish("HARNESS", "the window manager never brought the desktop up", 2)
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            finish("HARNESS", str(e), 2)
        time.sleep(7)

        # ---- self-test: proves the whole path (tab, address bar, server) ----
        mark = len(serial())
        ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("CF-READY-" + token, SELFTEST_BUDGET, mark):
            finish("HARNESS", "the self-test page never loaded -- nothing was measured", 2)

        gate_ok = True
        gate_failures = []

        # ---- the GATE: the synthetic fixture page --------------------------
        if not args.skip_fixture:
            mark, ok = load_page("http://10.0.2.2:%d/fx/inserted-datablock.html" % port)
            seg = serial(mark)
            fx = {"loaded": "CRASHFIX-PAGE-LOADED" in seg,
                  "exceptions": exceptions_in(seg),
                  "skipped_inserted": seg.count("skipping inserted <script"),
                  "exe_ran": "CRASHFIX-EXE-RAN" in seg,
                  "jstype_ran": "CRASHFIX-JSTYPE-RAN" in seg,
                  "module_ran": "CRASHFIX-MODULE-RAN" in seg,
                  "innerHTML_ran": "CRASHFIX-INNERHTML-RAN" in seg}
            rec["fixture"] = fx
            if args.expect_broken:
                # THE CONTROL'S ASSERTIONS. The red must be the DEFECT, not a
                # dead page: the data blocks execute and throw the two
                # recorded error shapes, nothing is skipped, and the
                # executable controls still ran -- proving the instrument can
                # see exactly the behaviour the fix removes.
                if not fx["loaded"]:
                    gate_failures.append("CONTROL: the fixture page never loaded at all")
                if not any("expecting ';'" in e for e in fx["exceptions"]):
                    gate_failures.append("CONTROL: the ld+json data block did not "
                                         "throw 'expecting ;' (the instrument cannot "
                                         "see the defect)")
                if not any("'%'" in e for e in fx["exceptions"]):
                    gate_failures.append("CONTROL: the %-template data block did not "
                                         "throw (the instrument cannot see the defect)")
                if fx["skipped_inserted"] != 0:
                    gate_failures.append("CONTROL: %d skipping lines on a build with "
                                         "the gate compiled out -- wrong build?"
                                         % fx["skipped_inserted"])
                gate_ok = not gate_failures
            else:
                if not fx["loaded"]:
                    gate_failures.append("the fixture page never logged CRASHFIX-PAGE-LOADED")
                if fx["exceptions"]:
                    gate_failures.append("%d JS exception(s) from the fixture page: %s"
                                         % (len(fx["exceptions"]), fx["exceptions"][:3]))
                if fx["skipped_inserted"] < 3:
                    gate_failures.append("only %d 'skipping inserted <script' lines (want >=3)"
                                         % fx["skipped_inserted"])
                if not fx["exe_ran"]:
                    gate_failures.append("inserted no-type script did NOT run (over-blocking, "
                                         "or script.text is not reaching the element's text)")
                if not fx["jstype_ran"]:
                    gate_failures.append("inserted text/javascript script did NOT run (over-blocking)")
                if fx["innerHTML_ran"]:
                    gate_failures.append("an innerHTML-injected script RAN (it never may)")
                gate_ok = not gate_failures

        # ---- the specimens: evidence, never assertions ---------------------
        for name in ("google-search", "douyin", "stripe", "secsdk-shape", "kimi-fm"):
            if only and name not in only:
                continue
            doc, _rw, smap, note = pages[name]
            main_page_doc[0] = doc
            main_page_doc[2] = smap
            main_page_name = name
            mark, ok = load_page("http://10.0.2.2:%d/page.html" % port,
                                 settle=30.0 if name == "stripe" else SETTLE)
            seg = serial(mark)
            rec["specimens"][name] = {"note": note, "loaded": ok,
                                      "exceptions": exceptions_in(seg)}
            if name == "secsdk-shape":
                rec["specimens"][name]["marker"] = "SECSDK-SHAPE" in seg
            if not ok:
                rec["specimens"][name]["note"] += " [navigation failed]"

        # how many subresource requests left the guest for the real network:
        # every line the browser logged as a dial that this server never saw
        log = serial()
        rec["served_live"] = sorted(set(re.findall(
            r"\[tls\] chain of \d+ verified for ([A-Za-z0-9.\-]+)", log)))

        if gate_ok:
            if args.skip_fixture:
                finish("REPLAYED", "specimens only", 0)
            finish("CONTROL-PASS" if args.expect_broken else "PASS",
                   "negative control red for the right reasons" if args.expect_broken
                   else "fixture gate green", 0)
        finish("FAIL", ("control: " if args.expect_broken else "") +
               ("; ".join(gate_failures) or "fixture failed"), 1)
    except SystemExit:
        raise
    except Exception as e:                                   # noqa: BLE001
        finish("HARNESS", "the harness itself raised: %r" % (e,), 2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
