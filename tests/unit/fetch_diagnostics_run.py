#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Check diagnostic evidence, with an optional ordinary local HTTP guest run.

The server never contacts an account or replays a captured request. Its private
fixture strings detect accidental logging of URLs, headers and bodies. Only a
successful page run can satisfy the missing-diagnostics negative control.
"""
import argparse
import hashlib
import http.server
import json
import pathlib
import re
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
CASES = {
    "application": ([200], False, 1),
    "http-refusal": ([403], False, 1),
    "no-response": ([], True, 1),
    "truncated": ([200], True, 1),
    "cors": ([200], True, 1),
    "redirect": ([302, 200], False, 2),
    "preflight": ([204, 200], False, 2),
}


def verify(log, negative=False, guest=False):
    terminal = "DIAG-GUEST-DONE failures=0"
    cases = dict(CASES)
    if not guest:
        cases["connect-start"] = ([], True, 1)
    # Count drift must not hide a failed fixture: host behavior is verified
    # independently of diagnostic shape rather than accepting any exit code.
    if not guest:
        assert re.search(r"fetch-diagnostics-behavior: \d+ checks, 0 failures", log), "HARNESS: behavior did not pass"
    else:
        assert terminal in log, "HARNESS: guest did not complete all cases"
        assert len(re.findall(r"DIAG-GUEST [a-z-]+: PASS", log)) == len(CASES), "HARNESS: guest case missing"
    assert not re.search(r"fixture_[a-z_]*secret", log), "private fixture value appeared in diagnostics"
    events = re.findall(r"\[webapi\] fetch-(request|response|detail) ([^\r\n]*)", log)
    if negative:
        assert not events, "negative control still emitted diagnostics"
        print("fetch-diagnostics-negctl: expected failure: request/response diagnostics absent; behavior passed")
        return
    segments = re.split(r"DIAG-CASE ([a-z-]+)[^\r\n]*", log)
    parts = dict(zip(segments[1::2], segments[2::2]))
    assert set(parts) == set(cases), "HARNESS: incorrect case set"
    for name, (statuses, failure, requests) in cases.items():
        rows = [(kind, dict(re.findall(r"([a-z_]+)=([^\s]+)", body)))
                for kind, body in re.findall(r"\[webapi\] fetch-(request|response|detail) ([^\r\n]*)", parts[name])]
        starts = [r for kind, r in rows if kind == "request"]
        replies = [r for kind, r in rows if kind == "response"]
        errors = [r for kind, r in rows if kind == "detail"]
        assert len(starts) == requests, name + ": incorrect request evidence"
        assert [int(r["status"]) for r in replies] == statuses, name + ": incorrect response evidence"
        assert len(errors) == int(failure), name + ": incorrect failure evidence"
        assert len({r["id"] for _, r in rows}) == 1, name + ": request identity changed across phases"
        if name == "preflight":
            assert [r["phase"] for r in starts] == ["1", "0"], "preflight phase missing"
        if name == "no-response":
            assert errors[0]["status"] == "0", "missing response looks like an HTTP status"
        if name in ("truncated", "cors"):
            assert errors[0]["status"] == "200", "failure lost the received status"
        if name == "cors":
            assert errors[0]["boundary"] == "cors-response", "CORS failure is unclassified"
        if name == "connect-start":
            assert errors[0]["boundary"] == "connect-start", "socket open failure is unclassified"
            assert errors[0]["error_code"] == "-5", "socket open failure lost the H1 transport error category"
    print(f"fetch-diagnostics: {len(cases)} cases preserve request identity, phase and status; fixture values absent")


def guest_run(args):
    sys.path.insert(0, str(ROOT / "tests/qmp"))
    from qmp_ui import Session
    out = pathlib.Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    serial = out / "serial.log"
    calls = []

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_OPTIONS(self):
            self.answer()

        def do_POST(self):
            self.rfile.read(int(self.headers.get("Content-Length", "0")))
            self.answer()

        def do_GET(self):
            self.answer()

        def answer(self):
            path = self.path.split("?", 1)[0]
            calls.append((path, self.command))
            if path == "/no-response":
                self.close_connection = True
                return
            code = 403 if path == "/http-refusal" else 302 if path == "/redirect" else 204 if self.command == "OPTIONS" else 200
            if path == "/":
                body = (ROOT / "tests/fixtures/browser/fetch-diagnostics.html").read_text().replace(
                    "CROSS_ORIGIN_PLACEHOLDER", f"http://10.0.2.2:{other.server_port}").encode()
                mime = "text/html"
            else:
                body = b'{"ok":false,"message":"fixture_body_secret"}'
                mime = "application/json"
            if code in (302, 204):
                body = b""
            self.send_response(code)
            self.send_header("Content-Type", mime)
            self.send_header("Content-Length", str(80 if path == "/truncated" else len(body)))
            self.send_header("Connection", "close")
            if path == "/redirect":
                self.send_header("Location", "/application?token=fixture_query_secret")
            if path == "/preflight":
                self.send_header("Access-Control-Allow-Origin", "*")
                self.send_header("Access-Control-Allow-Methods", "POST")
                self.send_header("Access-Control-Allow-Headers", "x-test")
            self.end_headers()
            self.wfile.write(b"cut" if path == "/truncated" else body)
            self.wfile.flush()
            self.close_connection = True

    first = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    other = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Handler)
    for server in (first, other):
        threading.Thread(target=server.serve_forever, daemon=True).start()
    artifacts = {}
    for name in (args.iso, args.disk):
        p = pathlib.Path(name).resolve()
        with p.open("rb") as stream:
            artifacts[str(p)] = hashlib.file_digest(stream, "sha256").hexdigest()
    (out / "artifacts.json").write_text(json.dumps(artifacts, indent=2))
    serial.write_text("")
    with tempfile.TemporaryDirectory(prefix="fetch-diag-", dir="/tmp") as tmp:
        sock = str(pathlib.Path(tmp) / "qmp.sock")
        command = ["qemu-system-x86_64", "-cpu", "max", "-cdrom", str(pathlib.Path(args.iso).resolve()),
                   "-drive", f"file={pathlib.Path(args.disk).resolve()},format=raw,if=none,id=hd0",
                   "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-snapshot", "-m", "1G",
                   "-smp", "4", "-accel", "tcg,thread=multi", "-vga", "none", "-device",
                   "virtio-gpu-pci,xres=1280,yres=800", "-display", "none", "-no-reboot",
                   "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
                   "-serial", "file:" + str(serial), "-qmp", "unix:" + sock + ",server,nowait"]
        with (out / "qemu.log").open("w") as stream:
            process = subprocess.Popen(command, stdout=stream, stderr=subprocess.STDOUT)
        try:
            def wait(marker, timeout=120):
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    log = serial.read_text(errors="replace")
                    if marker in log and "\n" in log[log.index(marker) + len(marker):]:
                        return log
                    if process.poll() is not None:
                        raise RuntimeError("HARNESS: QEMU exited")
                    time.sleep(.2)
                raise RuntimeError("HARNESS: missing guest marker " + marker)
            wait("desktop live")
            session = Session(sock, serial=str(serial))
            session.launch_app("browser")
            session.key_mods(("ctrl",), "l")
            session.typ(f"http://10.0.2.2:{first.server_port}/")
            session.key("ret")
            log = wait("DIAG-GUEST-DONE", 150)
            session.screendump(str(out / "result.ppm"))
            verify(log, guest=True)
            expected = {("/no-response", "POST"), ("/preflight", "OPTIONS"), ("/preflight", "POST")}
            assert expected.issubset(set(calls)), "HARNESS: server did not receive the expected methods"
            assert calls.count(("/no-response", "POST")) == 1, "POST was replayed"
            (out / "results.json").write_text(json.dumps({"cases": list(CASES), "failures": 0, "requests": calls}, indent=2))
        finally:
            process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait(timeout=5)
            for server in (first, other):
                server.shutdown(); server.server_close()
    for name, digest in artifacts.items():
        with open(name, "rb") as stream:
            assert hashlib.file_digest(stream, "sha256").hexdigest() == digest, "HARNESS: guest artifact changed"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--log")
    parser.add_argument("--negative-control", action="store_true")
    parser.add_argument("--guest", action="store_true")
    parser.add_argument("--iso")
    parser.add_argument("--disk")
    parser.add_argument("--out")
    args = parser.parse_args()
    if args.guest:
        guest_run(args)
    else:
        verify(pathlib.Path(args.log).read_text(), args.negative_control)


if __name__ == "__main__":
    main()
