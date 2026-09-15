#!/usr/bin/env python3
"""Stateful guest HTTP-cache regression; no live sites or host timing claims.

python3 tests/qmp/qmp_cache_invalidation.py --iso build/logit.iso \
    --disk build/disk.img --out build/cache-invalidation-guest.json

Use --expect-stale ONLY with a browser compiled BXFER_NO_CACHE_INVALIDATE.
The same driver must observe the stale painted document's DOM marker and the
missing server GET; a failed boot or a disabled cache never counts as control.
--serve-only exposes the target/control pages for manual guest navigation.
--selftest verifies only the Python server apparatus, never browser behavior.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import threading
import time
import urllib.request
import uuid

from qmp_ui import Session


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.version = self.gets = self.posts = 0
        self.nonce = uuid.uuid4().hex
        self.requests = []

    def snapshot(self):
        with self.lock:
            return dict(version=self.version, gets=self.gets, posts=self.posts,
                        nonce=self.nonce, requests=list(self.requests))


def server():
    state = State()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):
            pass

        def send(self, status, body=b"", cache="no-store"):
            self.send_response(status)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", cache)
            self.end_headers()
            if body:
                self.wfile.write(body)

        def do_GET(self):
            route = self.path.split("?", 1)[0]
            with state.lock:
                state.requests.append("GET " + self.path)
                if route == "/target":
                    state.gets += 1
                version = state.version
            if route == "/target":
                # The serial assertion reads the DOM's actual text; it does not
                # announce an expected version from a separate JS constant.
                body = ("<!doctype html><meta charset=utf-8><title>Cache target</title>"
                        "<h1 id=version>CACHE-GUEST-VERSION %d</h1>"
                        "<p>Cacheable document. Revisit before and after a mutation.</p>"
                        "<a href=/control>Open mutation control</a>"
                        "<script>console.log(document.getElementById('version').textContent"
                        "+' NONCE %s');</script>" % (version, state.nonce)).encode()
                self.send(200, body, "max-age=120")
            elif route == "/control":
                auto = "mutate();" if self.path.endswith("?auto=1") else ""
                body = ("<!doctype html><meta charset=utf-8><title>Mutation control</title>"
                        "<h1>HTTP cache mutation</h1><button onclick=mutate()>POST target</button>"
                        "<p id=result>Ready</p><a href=/target>Return to target</a>"
                        "<script>function mutate(){fetch('/target',{method:'POST'}).then(function(r){"
                        "document.getElementById('result').textContent='POST status '+r.status;"
                        "console.log('CACHE-GUEST-POST '+r.status+' NONCE %s');"
                        "},function(e){console.log('CACHE-GUEST-ERROR '+String(e));});}%s</script>"
                        % (state.nonce, auto)).encode()
                self.send(200, body)
            else:
                self.send(404, b"Unknown fixture")

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            if length:
                self.rfile.read(length)
            if self.path != "/target":
                self.send(404)
                return
            with state.lock:
                state.posts += 1
                state.version += 1
                state.requests.append("POST " + self.path)
            self.send(204)

    srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, state


def selftest(srv, state):
    base = "http://127.0.0.1:%d" % srv.server_port
    assert b"CACHE-GUEST-VERSION 0" in urllib.request.urlopen(base + "/target").read()
    assert b"mutate();" in urllib.request.urlopen(base + "/control?auto=1").read()
    req = urllib.request.Request(base + "/target", method="POST")
    assert urllib.request.urlopen(req).status == 204
    assert b"CACHE-GUEST-VERSION 1" in urllib.request.urlopen(base + "/target").read()
    assert state.snapshot()["gets"] == 2 and state.snapshot()["posts"] == 1
    print("cache-invalidation server selftest passed; no browser or QEMU exercised")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--iso", default="build/logit.iso")
    ap.add_argument("--disk", default="build/disk.img")
    ap.add_argument("--out", default="build/cache-invalidation-guest.json")
    ap.add_argument("--expect-stale", action="store_true")
    ap.add_argument("--serve-only", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    ap.add_argument("--boot-timeout", type=float, default=300)
    ap.add_argument("--load-timeout", type=float, default=120)
    args = ap.parse_args()
    srv, state = server()
    if args.selftest:
        try:
            selftest(srv, state)
        finally:
            srv.shutdown(); srv.server_close()
        return 0
    if args.serve_only:
        print("Guest: http://10.0.2.2:%d/target and /control" % srv.server_port, flush=True)
        try:
            threading.Event().wait()
        except KeyboardInterrupt:
            srv.shutdown(); srv.server_close()
        return 0
    out = Path(args.out).resolve()
    evidence = out.parent / (out.stem + "-evidence")
    evidence.mkdir(parents=True, exist_ok=True)
    serial_path = evidence / "serial.log"
    # QMP socket paths have a small Unix-domain limit; a short path stays under
    # it even when the requested evidence directory is in a deep checkout.
    qmp_path = "/tmp/logitos-cache-%s.sock" % state.nonce[:12]
    proc = None
    record = {"expect_stale": args.expect_stale, "server_port": srv.server_port,
              "serial": str(serial_path), "visits": [], "gate": "not-run"}
    code = 2

    def serial():
        try:
            return serial_path.read_text(errors="replace")
        except OSError:
            return ""

    def wait_for(needle, mark=0, timeout=None):
        # This clock bounds apparatus waiting only. No host duration is reported
        # as browser performance; the assertions are versions and request counts.
        end = time.monotonic() + (args.load_timeout if timeout is None else timeout)
        while time.monotonic() < end:
            if needle in serial()[mark:]:
                return
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited while waiting for " + needle)
            time.sleep(0.2)
        raise RuntimeError("missing guest marker: " + needle)

    try:
        for path in (args.iso, args.disk):
            if not Path(path).is_file() or Path(path).stat().st_size == 0:
                raise RuntimeError("missing/empty guest artifact: " + path)
        cmd = [os.environ.get("QEMU", "qemu-system-x86_64"), "-cpu", "max",
               "-cdrom", args.iso, "-drive", "file=%s,format=raw,if=none,id=hd0,file.locking=off" % args.disk,
               "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot",
               "-m", "1G", "-smp", "4", "-accel", "tcg,thread=multi", "-vga", "none",
               "-device", "virtio-gpu-pci,xres=1280,yres=800", "-display", "none", "-no-reboot",
               "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
               "-serial", "file:" + str(serial_path), "-qmp", "unix:%s,server,nowait" % qmp_path]
        with (evidence / "qemu.stderr").open("wb") as err:
            proc = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=err)
        wait_for("LOGIT_BOOT_OK", timeout=args.boot_timeout)
        wait_for("desktop live", timeout=args.boot_timeout)
        # desktop live precedes the initial Finder launch/dock animation.
        # Do not attribute that boot-time window to our later Browser click.
        time.sleep(3)
        ui = Session(qmp_path, serial=str(serial_path))
        ui.launch_app("browser")
        # A fresh profile opens New Tab without loading any document. Waiting
        # for load done here tests the apparatus, not the browser or cache.
        wait_for("[browser] sock probe:")
        ui.goto(40, 780)

        def visit(route, name, marker):
            mark = len(serial())
            ui.key_mods(["ctrl"], "l"); ui.typ("http://10.0.2.2:%d%s" % (srv.server_port, route)); ui.key("ret")
            wait_for("[browser] load:", mark)
            wait_for(marker + " NONCE " + state.nonce, mark)
            wait_for("[browser] load done", mark)
            shot = evidence / (name + ".ppm")
            ui.screendump(str(shot))
            record["visits"].append(dict(name=name, serial=serial()[mark:],
                                          server=state.snapshot(), screenshot=str(shot)))

        visit("/target", "initial", "CACHE-GUEST-VERSION 0")
        assert state.snapshot()["gets"] == 1, "first target GET was not observed"
        visit("/target", "warm", "CACHE-GUEST-VERSION 0")
        assert state.snapshot()["gets"] == 1, "warm navigation did not use cache; apparatus cannot test invalidation"
        visit("/control?auto=1", "mutation", "CACHE-GUEST-POST 204")
        assert state.snapshot()["posts"] == 1 and state.snapshot()["version"] == 1, "origin did not apply one POST"
        expected = 0 if args.expect_stale else 1
        visit("/target", "after", "CACHE-GUEST-VERSION %d" % expected)
        assert state.snapshot()["gets"] == (1 if args.expect_stale else 2), "target GET count disagrees with document version"
        record["gate"] = "negative-observed" if args.expect_stale else "pass"
        code = 0
    except (AssertionError, RuntimeError, OSError) as exc:
        record["gate"] = "fail"
        record["error"] = str(exc)
    finally:
        if proc is not None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        srv.shutdown(); srv.server_close()
        try:
            os.unlink(qmp_path)
        except FileNotFoundError:
            pass
        record["server"] = state.snapshot()
        out.write_text(json.dumps(record, indent=2) + "\n")
    print(json.dumps({"gate": record["gate"], "error": record.get("error"), "out": str(out)}))
    return code


if __name__ == "__main__":
    raise SystemExit(main())
