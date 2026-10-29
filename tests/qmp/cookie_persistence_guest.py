#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Real browser cookies across two QEMU boots against one writable disk copy.

The second page and all second-boot responses are read-only: they cannot seed
the expected state. Requests hit only this local fixture. Logs retain booleans
and guest markers, never Cookie headers. No -snapshot or snapshot rollback.
"""
import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import uuid

sys.path.insert(0, str(Path(__file__).resolve().parent))
from qmp_ui import Session


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for data in iter(lambda: f.read(1024 * 1024), b""):
            h.update(data)
    return h.hexdigest()


def export_system(source, helper, out):
    """Export the frozen system tree, excluding all browser profile bytes."""
    sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
    from disk_profile import CheckedImage
    import mkfs
    checked = out / "checked-system.img"
    subprocess.run([helper, str(Path(source).resolve()), str(checked)], check=True)
    try:
        image = CheckedImage(checked)
        destination = out / "repack-system"
        destination.mkdir()
        seen = set()

        def walk(number, host, root=False):
            if number in seen:
                raise RuntimeError("system export contains a multiply linked inode")
            seen.add(number)
            if image.inode(number)[0] == mkfs.T_DIR:
                host.mkdir(exist_ok=True)
                for name, child in image.directory(number).items():
                    if not (root and name == "browser"):
                        walk(child, host / name)
            else:
                host.write_bytes(image.payload(number))
        walk(image.sb[10], destination, True)
        (destination / "profile-rebuild-control.txt").write_text("synthetic updated system package\n")
        return destination
    finally:
        checked.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--iso", required=True)
    parser.add_argument("--disk", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--expect-missing", action="store_true")
    parser.add_argument("--rebuild-between", action="store_true",
                        help="Repackage the system between boots, retaining /browser")
    parser.add_argument("--snapshot-helper")
    args = parser.parse_args()
    if args.rebuild_between and (not args.snapshot_helper or args.expect_missing):
        parser.error("--rebuild-between requires --snapshot-helper and a positive image")
    out = Path(args.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    disk = out / "writable-disk.img"
    if disk.exists():
        raise RuntimeError("refusing to overwrite an existing persistence run disk")
    shutil.copyfile(args.disk, disk)
    packaged = export_system(args.disk, str(Path(args.snapshot_helper).resolve()), out) if args.rebuild_between else None
    token = uuid.uuid4().hex
    state = {"boot": 1, "observations": [], "seed_delivered": False}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            path = self.path.split("?", 1)[0]
            cookies = {}
            for field in self.headers.get("Cookie", "").split(";"):
                if "=" in field:
                    k, v = field.strip().split("=", 1)
                    cookies[k] = v
            if path == "/observe":
                row = {"boot": state["boot"],
                       "network_persistent": cookies.get("cp_net") == token,
                       "script_persistent": cookies.get("cp_js") == token,
                       "session": cookies.get("cp_session") == token}
                state["observations"].append(row)
                data = json.dumps(row).encode()
                mime = "application/json"
            else:
                seed = path == "/seed" and state["boot"] == 1 and not state["seed_delivered"]
                setup = ("document.cookie='cp_js=" + token + "; Max-Age=3600; Path=/; SameSite=Lax';"
                         "document.cookie='cp_session=" + token + "; Path=/; SameSite=Lax';") if seed else ""
                if seed and args.rebuild_between:
                    setup += "localStorage.setItem('profile_rebuild_test','" + token + "');"
                storage = ("+' storage='+(localStorage.getItem('profile_rebuild_test')==='" + token + "')") if args.rebuild_between else ""
                stage = "SEED" if state["boot"] == 1 else "REOPEN"
                data = ("<!doctype html><meta charset=utf-8><title>Cookie restart acceptance</title>"
                        "<h1 id=result>Waiting for browser cookie checks</h1><script>" + setup +
                        "var visible=document.cookie;var local=visible.indexOf('cp_js=" + token + "')>=0;"
                        "var session=visible.indexOf('cp_session=" + token + "')>=0;"
                        "var hidden=visible.indexOf('cp_net=')<0;"
                        "fetch('/observe',{credentials:'include'}).then(function(r){return r.json()}).then(function(r){"
                        "var text='COOKIE-GUEST " + stage + " local='+local+' session='+session+' hidden='+hidden+"
                        "' network='+r.network_persistent+' script='+r.script_persistent" + storage + ";"
                        "document.getElementById('result').textContent=text;console.log(text);"
                        "}).catch(function(){console.log('COOKIE-GUEST FIXTURE FETCH FAILED')});</script>").encode()
                mime = "text/html"
            self.send_response(200)
            self.send_header("Content-Type", mime)
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(data)))
            if path == "/seed" and state["boot"] == 1 and not state["seed_delivered"]:
                self.send_header("Set-Cookie", "cp_net=" + token + "; Max-Age=3600; Path=/; HttpOnly; SameSite=Lax")
                state["seed_delivered"] = True
            self.end_headers()
            self.wfile.write(data)

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    report = {"iso_sha256": digest(args.iso), "source_disk_sha256": digest(args.disk),
              "writable_disk": str(disk), "boots": [], "observations": state["observations"]}
    try:
        for boot in (1, 2):
            state["boot"] = boot
            serial = out / ("boot%d-serial.txt" % boot)
            serial.write_text("")
            with tempfile.TemporaryDirectory(prefix="ckp-qmp-", dir="/tmp") as tmp:
                sock = str(Path(tmp) / "qmp.sock")
                cmd = [os.environ.get("QEMU", "qemu-system-x86_64"), "-cpu", "max", "-cdrom", args.iso,
                       "-drive", "file=%s,format=raw,if=none,id=hd0" % disk,
                       "-device", "virtio-blk-pci,drive=hd0", "-boot", "d", "-m", "1G", "-smp", "4",
                       "-accel", "tcg,thread=multi", "-vga", "none", "-device", "virtio-gpu-pci,xres=1280,yres=800",
                       "-display", "none", "-no-reboot", "-netdev", "user,id=n0", "-device", "e1000,netdev=n0",
                       "-serial", "file:" + str(serial), "-qmp", "unix:" + sock + ",server,nowait"]
                with open(out / ("boot%d-qemu.txt" % boot), "w") as qlog:
                    process = subprocess.Popen(cmd, stdout=qlog, stderr=subprocess.STDOUT)
                    def wait(marker, start=0, seconds=150):
                        end = time.monotonic() + seconds
                        while time.monotonic() < end:
                            text = serial.read_text(errors="replace")[start:]
                            for line in text.splitlines():
                                if marker in line and text.endswith("\n"):
                                    return line
                            if process.poll() is not None:
                                raise RuntimeError("QEMU exited before " + marker)
                            time.sleep(0.2)
                        raise RuntimeError("guest marker missing: " + marker)
                    try:
                        wait("desktop live")
                        ui = Session(sock, serial=str(serial))
                        ui.launch_app("browser")
                        time.sleep(2)
                        start = len(serial.read_text(errors="replace"))
                        ui.key_mods(("ctrl",), "t", settle=0.3)
                        ui.typ("http://10.0.2.2:%d/%s" % (server.server_port, "seed" if boot == 1 else "check"))
                        ui.key("ret")
                        stage = "SEED" if boot == 1 else "REOPEN"
                        marker = wait("COOKIE-GUEST " + stage, start)
                        report["boots"].append({"boot": boot, "marker": marker})
                        ui.screendump(str(out / ("boot%d.ppm" % boot)))
                        if boot == 1:
                            assert "local=true session=true hidden=true network=true script=true" in marker, "seed did not reach both real Cookie doors"
                        else:
                            expected = "local=false session=false hidden=true network=false script=false" if args.expect_missing else "local=true session=false hidden=true network=true script=true"
                            assert expected in marker, "persistent cookie restart mismatch: " + marker
                        assert "Cookie snapshot could not be loaded" not in serial.read_text(errors="replace"), "guest store adapter failed"
                        assert "cookie persistence failed" not in serial.read_text(errors="replace"), "guest flush failed"
                        if args.rebuild_between:
                            assert "storage=true" in marker, "localStorage did not survive system repack"
                    finally:
                        # A process SIGTERM after the synchronous success marker
                        # cuts QEMU without asking the browser to run an exit hook.
                        process.terminate()
                        try:
                            process.wait(timeout=15)
                        except subprocess.TimeoutExpired:
                            process.kill(); process.wait(timeout=10)
            if boot == 1 and args.rebuild_between:
                report["before_rebuild_sha256"] = digest(disk)
                command = [sys.executable, str(Path(__file__).resolve().parents[2] / "tools/mkfs.py"),
                           "--preserve", "/browser", "--snapshot-helper", str(Path(args.snapshot_helper).resolve()),
                           str(disk), str(packaged) + ":/"]
                with open(out / "rebuild.log", "w") as log:
                    subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
                report["after_rebuild_sha256"] = digest(disk)
                assert report["before_rebuild_sha256"] != report["after_rebuild_sha256"], "system repack did not modify disk"
                report["rebuild_performed"] = True
        report["passed"] = True
        report["expect_missing"] = args.expect_missing
        print("cookie-persistence-guest: 2 boots passed (%s)" % ("system repack + localStorage + persistent + session + HttpOnly" if args.rebuild_between else "missing-cookie control" if args.expect_missing else "persistent + session + HttpOnly"))
    finally:
        server.shutdown()
        report["final_disk_sha256"] = digest(disk)
        (out / "results.json").write_text(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
