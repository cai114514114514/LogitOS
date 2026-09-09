#!/usr/bin/env python3
"""DOM-deficit census: what this browser builds and paints of a real page,
against headless Chrome on the same URL, one QEMU boot per site.

    # one site on the machine (writes <workdir>/<name>.json + serial + PNGs)
    python3 tests/qmp/qmp_attack_dom.py site --iso B/logit.iso --disk B/disk.img \
        --name github --url https://github.com/ --workdir B/atk-dom

    # every site in tests/fixtures/attack/dom/sites.tsv, five boots at a time
    python3 tests/qmp/qmp_attack_dom.py all --iso ... --disk ... --workdir B/atk-dom --jobs 5

    # the oracle: headless Chrome --dump-dom for every site, both User-Agents
    python3 tests/qmp/qmp_attack_dom.py chrome --workdir B/atk-dom

    # the census table: ours / Chrome per site, first cause per site
    python3 tests/qmp/qmp_attack_dom.py census --workdir B/atk-dom

THIS IS AN INSTRUMENT (rule 1: suspect the apparatus first). It fixes nothing,
asserts nothing about the browser, and exits 0 whether the site worked or not;
its exit code is about whether the MEASUREMENT happened. It reuses
qmp_site.py's proven pieces (the self-test page, the paced Ctrl+T/Ctrl+L
navigation, the serial parser, the host probe) and adds three things that
scoreboard row does not have:

  1. A GUEST-CLOCK DWELL. The owner's symptom is "a large amount of DOM is
     missing", and a DOM that arrives late looks identical to one that never
     arrives if the photograph is taken too early. The scoreboard dwells 15 s
     of HOST time, on a host that runs several QEMUs at once, so its 15 s is
     an unknown number of guest seconds. Here the pointer is nudged once per
     second at a parked spot (motions>0 defeats wm.c's idle gate, so the
     `[wm] perf t=<ms>` line prints every guest second) and the dwell is 20 s
     OF THAT CLOCK after `load done`, whatever the host is doing.
  2. THE DISPLAY LIST (about:boxes) beside the painted text (about:text): the
     nearest thing this browser has to a node count, because it has NO DOM
     count diagnostic at all -- grep 'printf' c/apps/browser/dom.c
     c/apps/browser/html_tree.c returns nothing. That absence is itself a
     finding of this census (see the report), and the proposed one line is
     stated there rather than added here, because this round adds files and
     edits none.
  3. THE ORACLE. Headless Chrome's --dump-dom on the same URL, run twice: once
     as Chrome, once carrying THIS browser's own User-Agent string, so "the
     site served us a smaller document" can be told apart from "we built less
     of the document we were served". Neither run mimics anything: the
     LogitOS-UA run is Chrome rendering what a site chooses to give LogitOS.

APPARATUS NOTES, MEASURED 2026-09-02 (Chrome 151.0.7922.175, this host):
  * Chrome's --dump-dom writes the DOM and then the process frequently never
    exits (three flag variants sat past 90 s with the profile already unloaded
    in the --v=1 log). Every oracle run is therefore killed after a wall
    budget and the buffered stdout kept; a dump counts only if it ends in
    </html>, otherwise the row says INCOMPLETE.
  * `timeout(1)` does not exist on this host; subprocess timeouts are used.
  * The pointer lives on the hardware cursor plane and is NOT in a
    screendump, so nothing here looks for it in pixels.
"""

import argparse
import concurrent.futures
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import unicodedata
from html.parser import HTMLParser

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from qmp_ui import Session, PPM                                    # noqa: E402
import qmp_site as S                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SITES = os.path.join(ROOT, "tests", "fixtures", "attack", "dom", "sites.tsv")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

LOAD_BUDGET = S.env_f("SITE_LOAD", 240)      # host seconds to wait for `load done`
DWELL_GUEST_MS = int(S.env_f("ATK_DWELL_MS", 20000))
DWELL_HOST_CAP = S.env_f("ATK_DWELL_CAP", 150)   # host seconds; a stalled guest clock is recorded, not waited on forever

PERF_RE = re.compile(r"\[wm\] perf t=(\d+)")
WA_RE = re.compile(r"\[wa\] t=(\d+) (?:\(\+\d+\) )?(\S+)(.*)$")


def load_sites(path, only=None):
    rows = []
    for ln in open(path, encoding="utf-8"):
        ln = ln.rstrip("\n")
        if not ln.strip() or ln.lstrip().startswith("#"):
            continue
        name, url = ln.split("\t")[:2]
        if only and name not in only:
            continue
        rows.append((name, url))
    return rows


def norm_chars(text):
    """Code points of whitespace-collapsed text: the ONE definition used for
    both sides. Ours arrives as UTF-8 text runs off the serial log, Chrome's
    as text nodes out of --dump-dom; counting bytes on one side and code
    points on the other would inflate every CJK page's deficit by up to 3x."""
    return len(re.sub(r"\s+", " ", text).strip())


def han_share(text):
    t = re.sub(r"\s+", "", text)
    if not t:
        return 0.0
    n = sum(1 for c in t if unicodedata.category(c)[0] == "L" and ord(c) > 0x2E7F)
    return round(n / len(t), 3)


# ----------------------------------------------------------------- guest side

def first_of(seq):
    return seq[0] if seq else None


def parse_guest(tail, before=None):
    """Everything the guest said between Enter and the about:text trigger,
    in ORDER, so 'first' means first.

    `before` is the serial log BEFORE Enter. browser_paint prints a painted-
    text summary only when the (runs, bytes) pair CHANGES, so a page that
    paints exactly what the empty tab painted -- nothing -- prints nothing,
    and 'no summary after the navigation' means 'the pair is still the last
    one printed', not 'unknown'. Measured on taobao: three exceptions, 32
    requests, load done at t=103650, and not one [dl] line after Enter; the
    last pair before Enter was (0, 0). Without this the row read None."""
    tail = tail.replace("\r\n", "\n")
    cut = tail.find("[browser] load: about:text")
    page = tail[:cut] if cut >= 0 else tail
    g = S.parse_serial(page)                  # exceptions (with stacks), load done, fetch failures
    lines = page.split("\n")

    # first-of-each, in serial order
    firsts = {"exception": None, "timer_exception": None, "webapi_exception": None,
              "console_error": None, "module_error": None, "watchdog": None,
              "fetch_stalled": None, "module_pending": None, "frame_refused": None,
              "worker": None, "failed_request": None, "script_lost": None,
              "fetch_failed": None, "cannot_fetch": None, "microtask_giveup": None}
    order = []
    page_bytes = None
    wa = []
    perf = []
    scripts = None
    resources = None
    heap = None
    for i, ln in enumerate(lines):
        def put(k, v):
            if firsts[k] is None:
                firsts[k] = v
                order.append((k, i, v))
        if "[browser] JS exception: " in ln:
            put("exception", ln.split("[browser] JS exception: ", 1)[1].strip())
        elif "[js] uncaught in timer: " in ln:
            put("timer_exception", ln.split("[js] uncaught in timer: ", 1)[1].strip())
        elif "[js] uncaught in " in ln or "[webapi] uncaught in " in ln:
            put("webapi_exception", ln.split("uncaught in ", 1)[1].strip())
        elif ln.startswith("[error] "):
            put("console_error", ln[8:].strip()[:300])
        elif "[browser] module exception in " in ln or "[browser] module rejected " in ln or "[js] module fetch FAILED" in ln:
            put("module_error", ln.strip()[:300])
        elif "[js] watchdog:" in ln:
            put("watchdog", ln.strip()[:300])
        elif "[browser] fetch stalled" in ln:
            put("fetch_stalled", ln.strip()[:300])
        elif "[js] module still pending" in ln:
            put("module_pending", ln.strip()[:300])
        elif "[frame] refused" in ln:
            put("frame_refused", ln.strip()[:300])
        elif ln.startswith("[worker"):
            put("worker", ln.strip()[:300])
        elif "[js] microtask queue did not drain" in ln:
            put("microtask_giveup", ln.strip()[:300])
        elif "[browser] fetch failed (status " in ln:
            v = ln.split("[browser] ", 1)[1].strip()
            put("fetch_failed", v); put("failed_request", v)
        elif "[browser] cannot fetch " in ln:
            v = ln.split("[browser] ", 1)[1].strip()
            put("cannot_fetch", v); put("failed_request", v)
        elif "[browser] script LOST" in ln or "[browser] script REFUSED" in ln or "[browser] inserted script" in ln:
            v = ln.split("[browser] ", 1)[1].strip()
            put("script_lost", v); put("failed_request", v)
        elif "[browser] page fetch failed" in ln:
            put("failed_request", ln.split("[browser] ", 1)[1].strip())
        m = re.search(r"\[browser\] (\S+) \((\d+) bytes\)", ln)
        if m and page_bytes is None:
            page_bytes = {"url": m.group(1), "bytes": int(m.group(2))}
        m = WA_RE.search(ln)
        if m:
            wa.append({"t": int(m.group(1)), "what": m.group(2), "rest": m.group(3).strip()[:120]})
        m = PERF_RE.search(ln)
        if m:
            perf.append(int(m.group(1)))
        m = re.search(r"\[browser\] scripts collected: (\d+) external classic, (\d+) external module, (\d+) inline", ln)
        if m:
            scripts = {"classic": int(m.group(1)), "module": int(m.group(2)), "inline": int(m.group(3))}
        m = re.search(r"\[browser\] resources: (\d+) from tab, (\d+) from network", ln)
        if m:
            resources = {"tab": int(m.group(1)), "net": int(m.group(2))}
        m = re.search(r"\[browser\] heap peak (\d+)K", ln)
        if m:
            heap = int(m.group(1))

    # painted text: the LAST dump before about:text, count and words from the SAME dump
    pairs = re.findall(
        r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte\(s\)([^\n]*)\n"
        r"(?:\[dl\] ---8<--- begin painted text\n(.*?)\[dl\] ---8<--- end painted text)?",
        page, re.S)
    text = None
    runs = tbytes = None
    truncated = False
    inherited = False
    if not pairs and before is not None:
        prev = re.findall(r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte\(s\)", before.replace("\r\n", "\n"))
        if prev:
            runs, tbytes = int(prev[-1][0]), int(prev[-1][1])
            text = "" if runs == 0 else None
            inherited = True
    if pairs:
        p = pairs[-1]
        runs, tbytes = int(p[0]), int(p[1])
        truncated = "TRUNCATED" in p[2]
        if p[3]:
            ws = []
            for l in p[3].split("\n"):
                if l.startswith("[dl] "):
                    body = l[5:]
                    sp = body.find(" ")
                    ws.append(body[sp + 1:] if sp >= 0 else body)
            text = "\n".join(ws)
    # THE PAINT HISTORY: every `[dl] painted text` summary in order. The
    # browser prints one whenever the pair changes, so this is the page's text
    # over time at zero cost -- and it is the only thing that separates
    # "built, then hidden" (stripe went 69 -> 38 -> 0 runs when React gave up
    # hydration) from "never built": both end at the same number.
    hist = [(int(a), int(b)) for a, b in re.findall(r"\[dl\] painted text: (\d+) run\(s\), (\d+) byte\(s\)", page)]
    peak = max(hist, key=lambda rb: rb[1]) if hist else None
    # the display list, from the part AFTER about:text (about:boxes is asked second)
    boxes = None
    bcut = tail.find("[browser] load: about:boxes")
    if bcut >= 0:
        # The serial console interleaves the window manager's per-second
        # `[wm] perf` line and the kernel's `[time]`/`[mm]` lines with the
        # dump -- measured: one `[dl] ... <#text` line had a whole [wm] line
        # glued onto it and became its own tag in the histogram. Only [dl]
        # lines are the display list; everything else is stripped first.
        # ... and the splice is MID-LINE, not between lines: the kernel's
        # serial writer interleaves at byte granularity, so the [wm] line
        # lands inside a [dl] line and the [dl] line's tail follows it.
        # Cutting the foreign line out (with its newline) rejoins the halves.
        bpart = re.sub(r"\[(?:wm|time|mm|tls|net|ip)\][^\n]*\n?", "", tail[bcut:])
        bpart = "\n".join(l for l in bpart.split("\n") if l.startswith("[dl] ") or l.startswith("[browser] "))
        m = re.search(r"\[dl\] boxes: (\d+) item\(s\)", bpart)
        if m:
            boxes = {"items": int(m.group(1)), "by_kind": {}, "by_tag": {}, "text_items": 0}
            for l in re.findall(r"\[dl\] (rect|text|img|video|ctrl|canvas)\s+(-?\d+),(-?\d+)\s+(\d+)x(\d+)\s+<([^>\n]*)>", bpart):
                boxes["by_kind"][l[0]] = boxes["by_kind"].get(l[0], 0) + 1
                boxes["by_tag"][l[5]] = boxes["by_tag"].get(l[5], 0) + 1
            boxes["text_items"] = boxes["by_kind"].get("text", 0)
            boxes["by_tag"] = dict(sorted(boxes["by_tag"].items(), key=lambda kv: -kv[1])[:12])
            boxes["shown_line"] = first_of(re.findall(r"\[dl\] ---8<--- end boxes \((\d+) shown of (\d+)\)", bpart))

    # every distinct exception, with its top frame and a callee name if the message names one
    exc = []
    for e in g["exceptions"]:
        top = e["stack"][0] if e["stack"] else ""
        callee = None
        m = re.search(r"(\S+) is not a function", e["message"]) or re.search(r"'([^']+)' is not a function", e["message"])
        if m:
            callee = m.group(1)
        m2 = re.search(r"cannot read propert(?:y|ies) '?([^' ]+)'? of (undefined|null)", e["message"], re.I)
        exc.append({"message": e["message"][:300], "top": top[:200], "count": e["count"],
                    "callee": callee, "property": m2.group(1) if m2 else None,
                    "stack": e["stack"][:6]})

    return {
        "load_done": g["load_done"], "requests": g["requests"], "dials": g["dials"],
        "reused": g["reused"], "modules": g["modules"], "modules_failed": g["modules_failed"],
        "scripts_collected": scripts, "resources": resources, "page_bytes": page_bytes,
        "heap_peak_k": heap, "page_fetch_failed": g["page_fetch_failed"],
        "app_fault": g["app_fault"], "panic": g["panic"],
        "n_exceptions": len(g["exceptions"]), "n_timer_exceptions": len(g["timer_exceptions"]),
        "n_module_exceptions": len(g["module_exceptions"]),
        "n_fetch_failed": len(g["fetch_failed"]), "n_cannot_fetch": len(g["cannot_fetch"]),
        "fetch_failed": g["fetch_failed"][:20], "cannot_fetch": g["cannot_fetch"][:20],
        "skipped_scripts": g["skipped_scripts"],
        "exceptions": exc[:12],
        "firsts": firsts, "first_order": order[:8],
        "text_runs": runs, "text_bytes": tbytes, "text_truncated": truncated,
        "text_chars": norm_chars(text) if text is not None else None,
        "text_inherited_from_last_print": inherited,
        "text_han_share": han_share(text) if text else 0.0,
        "text": text,
        "paint_history": hist[:40], "paint_peak_runs": peak[0] if peak else None,
        "paint_peak_bytes": peak[1] if peak else None,
        "boxes": boxes,
        "wa": wa[:40], "perf_t_first": perf[0] if perf else None, "perf_t_last": perf[-1] if perf else None,
    }


def run_site(args):
    name, url = args.name, args.url
    work = os.path.join(args.workdir, name)
    os.makedirs(work, exist_ok=True)
    out = os.path.join(args.workdir, "%s.json" % name)
    rec = {"name": name, "url": url, "started": time.strftime("%Y-%m-%dT%H:%M:%S"),
           "verdict": "HARNESS", "why": "did not run", "host": {}, "guest": {}, "pixels": {}}
    probe = {}
    rec["host"] = probe
    th = threading.Thread(target=S.host_probe, args=(url, probe), daemon=True)
    th.start()

    qmp_path = os.path.join(work, "qmp.sock")
    if os.path.exists(qmp_path):
        os.unlink(qmp_path)            # QEMU will not bind a path that exists (CLAUDE.md rule 1)
    serial_path = os.path.join(work, "serial.log")
    token = "%d" % (os.getpid() & 0xFFFF)
    pagebytes = (S.SELFTEST % token).encode()

    import http.server

    class Selftest(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.0"

        def do_GET(self):
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(pagebytes)))
            self.end_headers()
            try:
                self.wfile.write(pagebytes)
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
    qlog = open(os.path.join(work, "qemu.log"), "wb")
    for p, what in ((args.iso, "iso"), (args.disk, "disk")):
        if not os.path.exists(p) or os.path.getsize(p) == 0:
            rec["why"] = "the %s (%s) is missing or empty" % (what, p)
            json.dump(rec, open(out, "w"), indent=1, ensure_ascii=False)
            print(json.dumps({"name": name, "verdict": "HARNESS", "why": rec["why"]}))
            return 0
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
            shutil.copyfile(serial_path, os.path.join(args.workdir, "%s.serial.txt" % name))
        except OSError:
            pass
        th.join(timeout=50)
        hb = probe.pop("_body", None)
        if hb:
            with open(os.path.join(args.workdir, "%s.host.html" % name), "w", encoding="utf-8", errors="replace") as fh:
                fh.write(hb)
        try:
            proc.kill()
        except OSError:
            pass
        srv.shutdown()
        json.dump(rec, open(out, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
        print(json.dumps({"name": name, "verdict": verdict, "why": why}, ensure_ascii=False), flush=True)
        return 0

    try:
        if not wait_for("LOGIT_BOOT_OK", S.BOOT_BUDGET):
            return finish("HARNESS", "the kernel never printed LOGIT_BOOT_OK")
        if not wait_for("desktop live", 120):
            return finish("HARNESS", "the window manager never brought the desktop up")
        time.sleep(3)
        ui = Session(qmp_path, serial=serial_path)
        try:
            ui.launch_app("browser")
        except AssertionError as e:
            return finish("HARNESS", str(e))
        time.sleep(7)
        mark = len(serial())
        S.ctrl(ui, "t")
        ui.typ("http://10.0.2.2:%d/sb.html" % port)
        ui.key("ret")
        if not wait_for("SB-READY-" + token, S.SELFTEST_BUDGET, mark):
            return finish("HARNESS", "the self-test page never loaded")
        S.ctrl(ui, "t")
        time.sleep(2.0)
        ui.goto(*S.PARK)
        base_ppm = os.path.join(work, "base.ppm")
        ui.screendump(base_ppm, settle=0.8)
        base = PPM(base_ppm)

        typed = None
        for attempt in range(3):
            mark = len(serial())
            S.ctrl(ui, "l")
            ui.typ(url)
            t0 = time.time()
            ui.key("ret")
            if not wait_for("[browser] load: ", 8.0, mark):
                continue
            for ln in serial(mark).splitlines():
                i = ln.find("[browser] load: ")
                if i >= 0:
                    typed = ln[i + len("[browser] load: "):].strip()
                    break
            if typed == url or typed == url.rstrip("/"):
                break
            rec.setdefault("url_mistypes", []).append(typed)
            typed = None
        if typed is None:
            return finish("HARNESS", "the URL never reached the address bar intact (%r)" % rec.get("url_mistypes"))
        rec["url_confirmed"] = typed

        loaded = fetch_failed = False
        while time.time() - t0 < LOAD_BUDGET:
            s = serial(mark)
            if "[browser] load done:" in s:
                loaded = True
                break
            if "[browser] page fetch failed" in s:
                fetch_failed = True
                break
            if proc.poll() is not None:
                return finish("HARNESS", "QEMU exited during the load")
            time.sleep(0.6)
        load_host_s = round(time.time() - t0, 1)

        # ---- the guest-clock dwell: 20 s of `[wm] perf t=` after load done ----
        def last_perf(frm):
            ts = PERF_RE.findall(serial(frm))
            return int(ts[-1]) if ts else None
        dwell_mark = len(serial())
        t_start = None
        th0 = time.time()
        wiggle = 0
        while time.time() - th0 < DWELL_HOST_CAP:
            ui.goto(S.PARK[0] + (wiggle & 1), S.PARK[1], settle=0.05)
            wiggle += 1
            t = last_perf(dwell_mark)
            if t is not None:
                if t_start is None:
                    t_start = t
                elif t - t_start >= DWELL_GUEST_MS:
                    break
            if proc.poll() is not None:
                return finish("HARNESS", "QEMU exited during the dwell")
            time.sleep(0.5)
        t_end = last_perf(dwell_mark)
        rec["dwell"] = {"guest_ms": (t_end - t_start) if (t_start is not None and t_end is not None) else None,
                        "host_s": round(time.time() - th0, 1), "t_start": t_start, "t_end": t_end}

        after_ppm = os.path.join(work, "after.ppm")
        ui.goto(*S.PARK)
        ui.screendump(after_ppm, settle=0.8)
        after = PPM(after_ppm)
        changed = S.changed_pixels(base, after)
        S.ppm_to_png(after_ppm, os.path.join(args.workdir, "%s.png" % name))
        S.ppm_to_png(base_ppm, os.path.join(args.workdir, "%s.blank.png" % name))

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

        tail = serial(mark)
        g = parse_guest(tail, before=serial()[:mark])
        g["loaded"] = loaded
        g["load_host_s"] = load_host_s
        rec["guest"] = g
        rec["pixels"] = {"changed_px": changed,
                         "ink_px": after.dark_pixels(S.VIEWPORT, S.INK_THRESH),
                         "colours": S.viewport_colours(after)}
        th.join(timeout=50)
        host_ok = probe.get("ok", False) and 200 <= probe.get("status", 0) < 400
        inv = probe.get("inventory")
        gap = None
        if inv and g.get("requests") is not None and loaded:
            short = inv["mandatory"] - g["requests"]
            if short > 0:
                gap = {"mandatory": inv["mandatory"], "requested": g["requests"], "short_by": short,
                       "stylesheets": inv["stylesheets"], "script_src": inv["script_src"]}
        rec["subresources"] = {"host_inventory": inv, "gap": gap}

        nexc = g["n_exceptions"] + g["n_timer_exceptions"] + g["n_module_exceptions"]
        if g["panic"]:
            return finish("CRASH", "the kernel panicked")
        if g["app_fault"]:
            return finish("CRASH", "the browser process faulted: " + g["app_fault"])
        if fetch_failed:
            if not host_ok:
                return finish("NETWORK", "neither side could fetch it (%s)" % probe.get("error", probe.get("status")))
            return finish("FETCH-FAIL", "guest: %s; host HTTP %s" % (g["page_fetch_failed"], probe.get("status")))
        if not loaded:
            if not host_ok:
                return finish("NETWORK", "no load in %.0fs; host could not fetch it either" % LOAD_BUDGET)
            return finish("TIMEOUT", "no `load done` in %.0fs (host fetched in %.1fs)" % (LOAD_BUDGET, probe.get("elapsed", -1)))
        if changed <= S.BLANK_MAX:
            return finish("BLANK", "loaded, %d changed px, %d exceptions" % (changed, nexc))
        if nexc:
            return finish("ERRORS", "%d changed px, %d exception(s)" % (changed, nexc))
        if gap:
            return finish("GAP", "short by %d of %d mandatory subresources" % (gap["short_by"], gap["mandatory"]))
        return finish("PAINTED", "%d changed px, no exceptions, no gap" % changed)
    except SystemExit:
        raise
    except Exception as e:                                        # noqa: BLE001
        import traceback
        rec["traceback"] = traceback.format_exc()
        return finish("HARNESS", "driver error: %r" % (e,))


# ------------------------------------------------------------------- the runner

def run_all(args):
    rows = load_sites(args.sites, args.only.split(",") if args.only else None)
    os.makedirs(args.workdir, exist_ok=True)

    def one(row):
        name, url = row
        cmd = [sys.executable, os.path.abspath(__file__), "site", "--iso", args.iso, "--disk", args.disk,
               "--name", name, "--url", url, "--workdir", args.workdir, "--mem", args.mem]
        log = open(os.path.join(args.workdir, "%s.driver.log" % name), "wb")
        p = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        try:
            p.wait(timeout=args.per_site_timeout)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid, signal.SIGKILL)
            return name, "TIMEOUT-HARNESS"
        try:
            return name, json.load(open(os.path.join(args.workdir, "%s.json" % name)))["verdict"]
        except Exception:                                          # noqa: BLE001
            return name, "NO-RECORD"

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for name, v in ex.map(one, rows):
            print("%-20s %s" % (name, v), flush=True)


# ------------------------------------------------------------------ the oracle

LOGIT_UA = S.UA


def chrome_one(workdir, name, url, ua, wall, vt):
    prof = tempfile.mkdtemp(prefix="prof_%s_%s_" % (name, ua), dir=workdir)
    cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
           "--disable-extensions", "--hide-scrollbars", "--window-size=1280,800",
           "--user-data-dir=" + prof, "--virtual-time-budget=%d" % vt]
    if ua == "logit":
        cmd.append("--user-agent=" + LOGIT_UA)
    cmd += ["--dump-dom", url]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
    killed = False
    try:
        out, err = p.communicate(timeout=wall)
    except subprocess.TimeoutExpired:
        killed = True
        try:
            os.killpg(p.pid, signal.SIGKILL)
        except OSError:
            pass
        out, err = p.communicate()
    shutil.rmtree(prof, ignore_errors=True)
    html = out.decode("utf-8", "replace")
    path = os.path.join(workdir, "%s.%s.html" % (name, ua))
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(html)
    rec = {"name": name, "ua": ua, "url": url, "bytes": len(out),
           "complete": html.rstrip().lower().endswith("</html>"), "killed": killed,
           "elapsed_host_s": round(time.time() - t0, 1), "rc": p.returncode, "path": path}
    print(json.dumps({k: rec[k] for k in ("name", "ua", "bytes", "complete", "killed", "elapsed_host_s")}), flush=True)
    return rec


def run_chrome(args):
    if not os.path.exists(CHROME):
        print("SKIP: no headless Chrome at %s -- install Google Chrome to run the oracle" % CHROME)
        return 2
    rows = load_sites(args.sites, args.only.split(",") if args.only else None)
    os.makedirs(args.workdir, exist_ok=True)
    jobs = [(name, url, ua) for name, url in rows for ua in args.uas.split(",")]
    recs = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(lambda j: chrome_one(args.workdir, j[0], j[1], j[2], args.wall, args.vt), jobs):
            recs.append(r)
    json.dump(recs, open(os.path.join(args.workdir, "chrome_dumps.json"), "w"), indent=1)
    return 0



# --------------------------------------------------- the oracle, second door
#
# --dump-dom gives a DOM with no layout in it, and this browser's painted-text
# dump is VIEWPORT-CULLED (browser_paint.c: "a record built from the layout
# tree instead would report text that a clip, an opacity or a viewport cull
# threw away"). Comparing the two directly says a 24,000 px tall article is
# 98% missing when its first screen is at parity. So the oracle is asked
# THROUGH THE SAME PAGE the way our side is measured: a counting script run
# inside headless Chrome over --remote-debugging-pipe (fd 3 in, fd 4 out,
# NUL-framed JSON -- no websocket library, nothing the page can see) that
# reports elements, text nodes, rendered text, and the text whose boxes
# intersect a viewport sized like ours (css_viewport(win_w, win_h) =
# WINW x WINH = 1180 x 620, browser.c). Calibration, measured 2026-09-02:
# example.com 125 (Chrome) vs 127 (ours); the wikipedia control's first
# screen 1305 vs 1382.

COUNT_JS = r"""
(function(){
  var W = window.innerWidth, H = window.innerHeight;
  var elems = document.getElementsByTagName('*').length;
  var walker = document.createTreeWalker(document, NodeFilter.SHOW_TEXT);
  var texts = 0, vchars = 0, vrun = 0, rchars = 0, rrun = 0, sample = [];
  var r = document.createRange();
  function visible(el){
    for (var e = el; e && e.nodeType === 1; e = e.parentElement) {
      var cs = getComputedStyle(e);
      if (cs.display === 'none' || cs.visibility === 'hidden' || parseFloat(cs.opacity) === 0) return false;
      var tn = e.tagName; if (tn === 'SCRIPT' || tn === 'STYLE' || tn === 'NOSCRIPT' || tn === 'TEMPLATE') return false;
    }
    return true;
  }
  var n;
  while ((n = walker.nextNode())) {
    texts++;
    var s = n.nodeValue.replace(/\s+/g, ' ').trim();
    if (!s) continue;
    if (!n.parentElement || !visible(n.parentElement)) continue;
    r.selectNodeContents(n);
    var rects = r.getClientRects();
    if (!rects.length) continue;
    var b = r.getBoundingClientRect();
    if (b.width === 0 && b.height === 0) continue;
    rchars += s.length; rrun++;
    var inview = false;
    for (var i = 0; i < rects.length; i++) {
      var q = rects[i];
      if (q.right > 0 && q.bottom > 0 && q.left < W && q.top < H && q.width > 0 && q.height > 0) { inview = true; break; }
    }
    if (inview) { vchars += s.length; vrun++; if (sample.length < 60) sample.push(s.slice(0, 80)); }
  }
  var it = (document.body && document.body.innerText) ? document.body.innerText : '';
  return JSON.stringify({elements: elems, text_nodes: texts, rendered_runs: rrun, rendered_chars: rchars,
    viewport_runs: vrun, viewport_chars: vchars, innerText_chars: it.replace(/\s+/g,' ').trim().length,
    inner_w: W, inner_h: H, title: document.title, url: location.href, ready: document.readyState,
    scroll_h: document.documentElement.scrollHeight, sample: sample});
})()
"""


class CdpPipe:
    def __init__(self, ua, prof, w, h):
        r3, w3 = os.pipe()
        r4, w4 = os.pipe()
        cmd = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run", "--no-default-browser-check",
               "--disable-extensions", "--hide-scrollbars", "--window-size=%d,%d" % (w, h),
               "--user-data-dir=" + prof, "--remote-debugging-pipe"]
        if ua == "logit":
            cmd.append("--user-agent=" + LOGIT_UA)
        cmd.append("about:blank")
        sh = "exec \"$@\" 3<&%d 4>&%d" % (r3, w4)
        self.p = subprocess.Popen(["/bin/sh", "-c", sh, "sh"] + cmd, pass_fds=(r3, w4),
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        os.close(r3); os.close(w4)
        self.w = w3; self.r = r4
        self.buf = b""; self.next_id = 0

    def send(self, method, params=None, session=None):
        self.next_id += 1
        m = {"id": self.next_id, "method": method, "params": params or {}}
        if session:
            m["sessionId"] = session
        os.write(self.w, json.dumps(m).encode() + b"\0")
        return self.next_id

    def recv(self, timeout):
        import select
        end = time.time() + timeout
        while True:
            i = self.buf.find(b"\0")
            if i >= 0:
                msg = self.buf[:i]; self.buf = self.buf[i + 1:]
                return json.loads(msg.decode("utf-8", "replace"))
            left = end - time.time()
            if left <= 0:
                return None
            rl, _, _ = select.select([self.r], [], [], min(left, 1.0))
            if rl:
                d = os.read(self.r, 1 << 16)
                if not d:
                    return None
                self.buf += d

    def wait_id(self, mid, timeout):
        end = time.time() + timeout
        while time.time() < end:
            m = self.recv(end - time.time())
            if m is None:
                return None
            if m.get("id") == mid:
                return m
        return None

    def kill(self):
        try:
            os.killpg(self.p.pid, signal.SIGKILL)
        except OSError:
            pass


def cdp_one(workdir, name, url, ua, settle=15.0, load_wait=60.0, w=1180, h=707):
    """--window-size includes 87 px the headless shell keeps (measured:
    655 -> inner 568), so 1180x707 yields inner 1180x620."""
    prof = tempfile.mkdtemp(prefix="cdp_%s_%s_" % (name, ua), dir=workdir)
    rec = {"name": name, "ua": ua, "url": url}
    t0 = time.time()
    c = CdpPipe(ua, prof, w, h)
    try:
        m = c.wait_id(c.send("Target.getTargets"), 20)
        if not m:
            rec["error"] = "no CDP answer"
            return rec
        tid = [t for t in m["result"]["targetInfos"] if t["type"] == "page"][0]["targetId"]
        sid = c.wait_id(c.send("Target.attachToTarget", {"targetId": tid, "flatten": True}), 20)["result"]["sessionId"]
        c.wait_id(c.send("Page.enable", session=sid), 10)
        c.wait_id(c.send("Runtime.enable", session=sid), 10)
        nav = c.wait_id(c.send("Page.navigate", {"url": url}, session=sid), 30)
        rec["nav"] = (nav or {}).get("result")
        end = time.time() + load_wait
        loaded = False
        while time.time() < end:
            ev = c.recv(end - time.time())
            if ev is None:
                break
            if ev.get("method") == "Page.loadEventFired":
                loaded = True
                break
        rec["load_event"] = loaded
        rec["load_s"] = round(time.time() - t0, 1)
        end = time.time() + settle
        while time.time() < end:
            if c.recv(end - time.time()) is None:
                break
        m = c.wait_id(c.send("Runtime.evaluate", {"expression": COUNT_JS, "returnByValue": True}, session=sid), 60)
        try:
            rec.update(json.loads(m["result"]["result"]["value"]))
        except Exception:                                          # noqa: BLE001
            rec["error"] = "evaluate: %r" % (m,)
        m = c.wait_id(c.send("Runtime.evaluate", {"expression": "document.documentElement.outerHTML", "returnByValue": True}, session=sid), 60)
        try:
            html = m["result"]["result"]["value"]
            with open(os.path.join(workdir, "%s.%s.cdp.html" % (name, ua)), "w", encoding="utf-8") as fh:
                fh.write(html)
            rec["html_bytes"] = len(html.encode("utf-8"))
        except Exception:                                          # noqa: BLE001
            pass
    except Exception as e:                                         # noqa: BLE001
        rec["error"] = "%s: %s" % (type(e).__name__, e)
    finally:
        c.kill()
        shutil.rmtree(prof, ignore_errors=True)
    rec["elapsed_s"] = round(time.time() - t0, 1)
    return rec


def run_cdp(args):
    if not os.path.exists(CHROME):
        print("SKIP: no headless Chrome at %s -- install Google Chrome to run the oracle" % CHROME)
        return 2
    rows = load_sites(args.sites, args.only.split(",") if args.only else None)
    os.makedirs(args.workdir, exist_ok=True)
    jobs = [(name, url, ua) for name, url in rows for ua in args.uas.split(",")]
    out = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(lambda j: cdp_one(args.workdir, j[0], j[1], j[2]), jobs):
            out.append(r)
            s = dict(r); s.pop("sample", None)
            print(json.dumps(s, ensure_ascii=False), flush=True)
    json.dump(out, open(os.path.join(args.workdir, "cdp_all.json"), "w"), indent=1, ensure_ascii=False)
    return 0


SKIP_TAGS = {"script", "style", "template", "noscript", "svg", "head", "title", "iframe"}
VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "param", "source", "track", "wbr"}


class DomCounter(HTMLParser):
    """Counts a --dump-dom serialisation. 'Visible' text = text nodes outside
    head/script/style/template/noscript/svg/title/iframe and outside any
    ancestor carrying `hidden`, inline display:none or visibility:hidden. Text
    hidden by a CLASS rule is still counted -- the dump carries no computed
    style -- so Chrome's number is an upper bound on what it painted."""

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.elems = self.texts = self.vis_texts = self.chars = 0
        self.stack = []
        self.tags = {}
        self.words = []

    def hidden(self):
        return any(h for _, h in self.stack)

    def handle_starttag(self, tag, attrs):
        self.elems += 1
        self.tags[tag] = self.tags.get(tag, 0) + 1
        a = dict(attrs)
        st = (a.get("style") or "").replace(" ", "").lower()
        h = tag in SKIP_TAGS or "hidden" in a or "display:none" in st or "visibility:hidden" in st
        if tag not in VOID:
            self.stack.append((tag, h))

    def handle_startendtag(self, tag, attrs):
        self.elems += 1
        self.tags[tag] = self.tags.get(tag, 0) + 1

    def handle_endtag(self, tag):
        for i in range(len(self.stack) - 1, -1, -1):
            if self.stack[i][0] == tag:
                del self.stack[i:]
                break

    def handle_data(self, data):
        self.texts += 1
        if self.hidden():
            return
        s = re.sub(r"\s+", " ", data).strip()
        if s:
            self.vis_texts += 1
            self.chars += len(s)
            self.words.append(s)


def count_dump(path):
    html = open(path, encoding="utf-8", errors="replace").read()
    c = DomCounter()
    c.feed(html)
    text = "\n".join(c.words)
    return {"elements": c.elems, "text_nodes": c.texts, "visible_text_nodes": c.vis_texts,
            "visible_chars": c.chars, "bytes": len(html.encode("utf-8")),
            "complete": html.rstrip().lower().endswith("</html>"),
            "han_share": han_share(text), "text": text}


# ------------------------------------------------------------------ the census

def first_cause(g, rec):
    """Name the FIRST thing that went wrong, in the three families the round
    asked for: DIED (a script threw), WAITED (nothing threw, something never
    arrived), HIDDEN/OTHER (built and not painted, or the site refused us).
    Order of the serial log decides 'first', never severity."""
    v = rec.get("verdict")
    if v in ("HARNESS", "NETWORK"):
        return v, rec.get("why", "")
    if v == "FETCH-FAIL":
        return "REFUSED/FETCH-FAIL", rec.get("why", "")
    if v == "CRASH":
        return "DIED:process", rec.get("why", "")
    host = rec.get("host") or {}
    if host.get("status") and host["status"] >= 400:
        return "REFUSED:http-%s" % host["status"], "host probe with our UA got HTTP %s" % host["status"]
    f = g.get("firsts") or {}
    order = g.get("first_order") or []
    pk, fin = g.get("paint_peak_bytes"), g.get("text_bytes")
    if pk and fin is not None and fin < pk // 2:
        return "HIDDEN:built-then-hidden", "painted text peaked at %d bytes and ended at %d (history %s)" % (pk, fin, g.get("paint_history"))
    for kind, _i, val in order:
        if kind in ("exception", "timer_exception", "webapi_exception", "module_error", "worker", "frame_refused"):
            return "DIED:" + kind, val
        if kind in ("watchdog", "microtask_giveup"):
            return "DIED:" + kind, val
        if kind in ("fetch_stalled", "module_pending"):
            return "WAITED:" + kind, val
        if kind in ("failed_request", "fetch_failed", "cannot_fetch", "script_lost"):
            return "WAITED:request-failed", val
        if kind == "console_error":
            return "DIED:console_error", val
    if not g.get("load_done"):
        return "WAITED:no-load-done", "no `load done` line"
    return "NONE", "nothing on the serial log names a cause"


def run_census(args):
    rows = load_sites(args.sites, args.only.split(",") if args.only else None)
    table = []
    for name, url in rows:
        rp = os.path.join(args.workdir, "%s.json" % name)
        rec = json.load(open(rp, encoding="utf-8")) if os.path.exists(rp) else {"verdict": "NO-RECORD", "guest": {}}
        g = rec.get("guest") or {}
        ch = {}
        for ua in ("chrome", "logit"):
            p = os.path.join(args.workdir, "%s.%s.html" % (name, ua))
            ch[ua] = count_dump(p) if os.path.exists(p) and os.path.getsize(p) > 0 else None
        ours_chars = g.get("text_chars")
        cdp = {}
        cp = os.path.join(args.workdir, "cdp_all.json")
        if os.path.exists(cp):
            for c in json.load(open(cp, encoding="utf-8")):
                if c.get("name") == name:
                    cdp[c["ua"]] = c
        row = {"name": name, "url": url, "verdict": rec.get("verdict"),
               "cdp_chrome_viewport_chars": (cdp.get("chrome") or {}).get("viewport_chars"),
               "cdp_chrome_rendered_chars": (cdp.get("chrome") or {}).get("rendered_chars"),
               "cdp_chrome_elements": (cdp.get("chrome") or {}).get("elements"),
               "cdp_chrome_text_nodes": (cdp.get("chrome") or {}).get("text_nodes"),
               "cdp_chrome_title": (cdp.get("chrome") or {}).get("title"),
               "cdp_logit_viewport_chars": (cdp.get("logit") or {}).get("viewport_chars"),
               "cdp_logit_rendered_chars": (cdp.get("logit") or {}).get("rendered_chars"),
               "cdp_logit_elements": (cdp.get("logit") or {}).get("elements"),
               "cdp_logit_text_nodes": (cdp.get("logit") or {}).get("text_nodes"),
               "cdp_logit_title": (cdp.get("logit") or {}).get("title"),
               "ours_text_runs": g.get("text_runs"), "ours_text_chars": ours_chars,
               "ours_text_truncated": g.get("text_truncated"),
               "ours_boxes": (g.get("boxes") or {}).get("items"),
               "ours_text_boxes": (g.get("boxes") or {}).get("text_items"),
               "chrome_elems": ch["chrome"]["elements"] if ch["chrome"] else None,
               "chrome_chars": ch["chrome"]["visible_chars"] if ch["chrome"] else None,
               "chrome_complete": ch["chrome"]["complete"] if ch["chrome"] else None,
               "logitua_elems": ch["logit"]["elements"] if ch["logit"] else None,
               "logitua_chars": ch["logit"]["visible_chars"] if ch["logit"] else None,
               "load_done": g.get("load_done"), "requests": g.get("requests"),
               "modules": g.get("modules"), "modules_failed": g.get("modules_failed"),
               "n_exceptions": g.get("n_exceptions"), "n_fetch_failed": g.get("n_fetch_failed"),
               "dwell_guest_ms": (rec.get("dwell") or {}).get("guest_ms"),
               "host_status": (rec.get("host") or {}).get("status")}
        for ua in ("chrome", "logit"):
            c = row["%s_chars" % ("chrome" if ua == "chrome" else "logitua")]
            row["ratio_%s" % ua] = round(ours_chars / c, 3) if (ours_chars is not None and c) else None
            v = row["cdp_%s_viewport_chars" % ua]
            row["ratio_viewport_%s" % ua] = round(ours_chars / v, 3) if (ours_chars is not None and v) else None
        row["first_cause"], row["first_cause_detail"] = first_cause(g, rec)
        table.append(row)
    json.dump(table, open(os.path.join(args.workdir, "census.json"), "w"), indent=1, ensure_ascii=False)
    hdr = "%-17s %-9s %6s %7s %7s %7s %7s %7s %6s %6s  %s"
    print(hdr % ("site", "verdict", "ours", "chrVP", "chrALL", "lgVP", "lgALL", "chrEL", "r/chr", "r/lg", "first cause"))
    for r in table:
        print(hdr % (r["name"], r["verdict"], r["ours_text_chars"],
                     r["cdp_chrome_viewport_chars"], r["cdp_chrome_rendered_chars"],
                     r["cdp_logit_viewport_chars"], r["cdp_logit_rendered_chars"], r["cdp_chrome_elements"],
                     r["ratio_viewport_chrome"] if r["ratio_viewport_chrome"] is not None else "-",
                     r["ratio_viewport_logit"] if r["ratio_viewport_logit"] is not None else "-",
                     "%s  %s" % (r["first_cause"], (r["first_cause_detail"] or "")[:80])))
    return 0


def run_reparse(args):
    """Re-derive every record's guest section from its saved serial log.
    The log is the measurement; the JSON is a reading of it, and a reading
    can be redone without booting anything."""
    rows = load_sites(args.sites, args.only.split(",") if args.only else None)
    for name, _url in rows:
        rp = os.path.join(args.workdir, "%s.json" % name)
        sp = os.path.join(args.workdir, "%s.serial.txt" % name)
        if not (os.path.exists(rp) and os.path.exists(sp)):
            continue
        rec = json.load(open(rp, encoding="utf-8"))
        text = open(sp, encoding="utf-8", errors="replace").read()
        url = rec.get("url_confirmed") or rec["url"]
        i = text.find("[browser] load: " + url)
        tail = text[i:] if i >= 0 else text
        old = rec.get("guest") or {}
        g = parse_guest(tail, before=text[:i] if i >= 0 else None)
        g["loaded"] = old.get("loaded"); g["load_host_s"] = old.get("load_host_s")
        rec["guest"] = g
        json.dump(rec, open(rp, "w", encoding="utf-8"), indent=1, ensure_ascii=False)
        print("%-20s text_chars=%s boxes=%s first=%s" % (name, g["text_chars"], (g["boxes"] or {}).get("items"), g["first_order"][:1]))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="mode", required=True)
    s = sub.add_parser("site")
    s.add_argument("--iso", required=True); s.add_argument("--disk", required=True)
    s.add_argument("--name", required=True); s.add_argument("--url", required=True)
    s.add_argument("--workdir", required=True); s.add_argument("--mem", default="1G")
    a = sub.add_parser("all")
    a.add_argument("--iso", required=True); a.add_argument("--disk", required=True)
    a.add_argument("--workdir", required=True); a.add_argument("--sites", default=SITES)
    a.add_argument("--only", default=""); a.add_argument("--jobs", type=int, default=5)
    a.add_argument("--mem", default="1G"); a.add_argument("--per-site-timeout", type=float, default=900.0)
    c = sub.add_parser("chrome")
    c.add_argument("--workdir", required=True); c.add_argument("--sites", default=SITES)
    c.add_argument("--only", default=""); c.add_argument("--jobs", type=int, default=3)
    c.add_argument("--uas", default="chrome,logit"); c.add_argument("--wall", type=float, default=100.0)
    c.add_argument("--vt", type=int, default=10000)
    d = sub.add_parser("cdp")
    d.add_argument("--workdir", required=True); d.add_argument("--sites", default=SITES)
    d.add_argument("--only", default=""); d.add_argument("--jobs", type=int, default=2)
    d.add_argument("--uas", default="chrome,logit")
    z = sub.add_parser("census")
    z.add_argument("--workdir", required=True); z.add_argument("--sites", default=SITES)
    z.add_argument("--only", default="")
    r = sub.add_parser("reparse")
    r.add_argument("--workdir", required=True); r.add_argument("--sites", default=SITES)
    r.add_argument("--only", default="")
    args = ap.parse_args()
    if args.mode == "reparse":
        sys.exit(run_reparse(args))
    if args.mode == "cdp":
        sys.exit(run_cdp(args))
    if args.mode == "site":
        sys.exit(run_site(args))
    if args.mode == "all":
        sys.exit(run_all(args))
    if args.mode == "chrome":
        sys.exit(run_chrome(args))
    if args.mode == "census":
        sys.exit(run_census(args))


if __name__ == "__main__":
    main()
