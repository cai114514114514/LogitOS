#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Local two-page Document.referrer acceptance on the shipping browser.

No public script, authentication, or server submission. The local server only
records whether a Referer field exists; values are never retained or printed.
The existing loader suppresses it, so the truthful DOM result is empty.
"""
import argparse
import http.server
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_ui import Session


def page(stage):
    return ("<!doctype html><meta charset=utf-8><title>Document referrer acceptance</title>"
            "<h1 id=result>Checking document metadata</h1><script>"
            "var stage='" + stage + "';var typed=typeof document.referrer==='string';"
            "var empty=document.referrer==='';var readonly=false;"
            "try{document.referrer='https://invented.example/';readonly=document.referrer===''}catch(e){}"
            "delete document.referrer;"
            "var detached=document.implementation.createHTMLDocument('local').referrer==='';"
            "var parsed=new DOMParser().parseFromString('<p>local</p>','text/html').referrer==='';"
            "var stable=true;if(stage==='PAGE'){history.pushState(null,'','/changed#one');"
            "location.hash='two';stable=document.referrer==='';}"
            "function finish(ok){var text='REFERRER-GUEST '+stage+' type='+typed+' empty='+empty+"
            "' readonly='+readonly+' promise='+ok+' detached='+detached+' parsed='+parsed+' stable='+stable;"
            "document.getElementById('result').textContent=text;console.log(text);"
            "if(stage==='DIRECT' && ok)setTimeout(function(){location.href='/destination'},300);}"
            "Promise.resolve().then(function(){return document.referrer.substr(0,100)}).then("
            "function(s){finish(s==='')},function(){finish(false)});</script>").encode()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--expect-missing", action="store_true")
    args = parser.parse_args()
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / "serial.log"
    serial.write_text("")
    report = {"passed": False, "markers": [], "requests": [],
              "boundary": "no recorded navigation referrer; outgoing Referer suppressed"}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            if path not in ("/source", "/destination"):
                self.send_error(404)
                return
            report["requests"].append({"page": path, "referer_present": "Referer" in self.headers})
            data = page("DIRECT" if path == "/source" else "PAGE")
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        with tempfile.TemporaryDirectory(prefix="referrer-qmp-", dir="/tmp") as tmp:
            sock = str(Path(tmp) / "qmp.sock")
            cmd = [os.environ.get("QEMU", "qemu-system-x86_64"), "-cpu", "max", "-cdrom", args.iso,
                   "-drive", "file=%s,format=raw,if=none,id=hd0" % args.disk,
                   "-device", "virtio-blk-pci,drive=hd0", "-snapshot", "-boot", "d", "-m", "1G", "-smp", "4",
                   "-accel", "tcg,thread=multi", "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
                   "-display", "none", "-no-reboot", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
                   "-serial", "file:" + str(serial), "-qmp", "unix:" + sock + ",server,nowait"]
            with open(out / "qemu.log", "w") as qlog:
                process = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)
                def wait(marker, seconds=150):
                    end = time.monotonic() + seconds
                    while time.monotonic() < end:
                        for line in serial.read_text(errors="replace").splitlines():
                            if line.startswith(marker):
                                return line
                        if process.poll() is not None:
                            raise RuntimeError("QEMU exited before " + marker)
                        time.sleep(0.2)
                    raise RuntimeError("guest marker missing: " + marker)
                try:
                    # Startup has a variable serial prefix; wait only for the
                    # fixed kernel marker before using the ordinary UI path.
                    end = time.monotonic() + 150
                    while "desktop live" not in serial.read_text(errors="replace"):
                        if time.monotonic() >= end or process.poll() is not None:
                            raise RuntimeError("desktop did not start")
                        time.sleep(0.2)
                    ui = Session(sock, serial=str(serial))
                    ui.launch_app("browser")
                    time.sleep(2)
                    ui.key_mods(("ctrl",), "t", settle=0.3)
                    ui.typ("http://10.0.2.2:%d/source" % server.server_port)
                    ui.key("ret")
                    direct = wait("REFERRER-GUEST DIRECT ")
                    report["markers"].append(direct)
                    good = "type=true empty=true readonly=true promise=true detached=true parsed=true stable=true"
                    if args.expect_missing:
                        assert "type=false" in direct and "promise=false" in direct, "missing-getter control did not fail"
                    else:
                        assert direct.endswith(good), "direct Document metadata failed"
                        linked = wait("REFERRER-GUEST PAGE ")
                        report["markers"].append(linked)
                        assert linked.endswith(good), "page navigation Document metadata failed"
                        assert len(report["requests"]) == 2, "actual second page navigation missing"
                    assert not any(r["referer_present"] for r in report["requests"]), "DOM must match transport suppression"
                    ui.screendump(str(out / "page.ppm"))
                    report["passed"] = True
                    report["expect_missing"] = args.expect_missing
                    print("document-referrer-guest: " + ("missing-getter control observed" if args.expect_missing else "two page lifecycle passed"))
                finally:
                    process.terminate()
                    try:
                        process.wait(timeout=15)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait(timeout=10)
    finally:
        server.shutdown()
        (out / "results.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
