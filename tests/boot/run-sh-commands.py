#!/usr/bin/env python3
"""Run /bin/sh -c command lists on the real guest and inspect guest-written files."""

import argparse
import datetime
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]


def filesystem_reader():
    path = ROOT / "tools/license_audit.py"
    spec = importlib.util.spec_from_file_location("sh_commands_fs", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module._LogitFS


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    build = args.build.resolve()
    out_root = args.out.resolve()
    out_root.mkdir(parents=True, exist_ok=True)
    run = out_root / (datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%dT%H%M%S%f") + f"-{os.getpid()}")
    run.mkdir()

    qemu = os.environ.get("QEMU", "qemu-system-x86_64")
    if not shutil.which(qemu):
        print(f"SKIP: test-sh-commands-os needs {qemu}; run this guest gate on a host with QEMU")
        return 77

    disk = run / "disk.img"
    files = [f"{build}/{name}.aex:/bin/{name}" for name in ("login", "sh", "echo", "false")]
    files += [f"{ROOT}/fsroot/fonts/{name}.ttf:/fonts/{name}.ttf" for name in ("ui", "mono")]
    with (run / "mkfs.log").open("w") as log:
        subprocess.run([sys.executable, "tools/mkfs.py", str(disk), *files], cwd=ROOT,
                       stdout=log, check=True)

    LogitFS = filesystem_reader()
    source_hash = digest((build / "sh.aex").read_bytes())
    fs = LogitFS(disk)
    try:
        inode = fs.lookup("/bin/sh")
        if inode is None or digest(fs.read_file(inode)) != source_hash:
            raise AssertionError("packed /bin/sh differs from this isolated build")
    finally:
        fs.close()

    command = [qemu, "-cpu", os.environ.get("QEMU_CPU", "max"), "-smp", "4", "-m", "512M",
               "-accel", "tcg,thread=multi", "-vga", "none", "-device", "virtio-gpu-pci",
               "-display", "none", "-no-reboot", "-cdrom", str(build / "logit.iso"), "-boot", "d",
               "-drive", f"file={disk},format=raw,if=none,id=hd0,file.locking=off",
               "-device", "virtio-blk-pci,drive=hd0", "-serial", "stdio"]
    (run / "qemu-command.txt").write_text(" ".join(command) + "\n")
    guest = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, cwd=ROOT)
    data = bytearray()
    os.set_blocking(guest.stdout.fileno(), False)

    def read_until(marker, deadline):
        while time.monotonic() < deadline:
            if marker in data:
                return
            if guest.poll() is not None:
                raise RuntimeError(f"QEMU exited before {marker!r}")
            try:
                chunk = os.read(guest.stdout.fileno(), 65536)
            except BlockingIOError:
                chunk = b""
            if chunk:
                data.extend(chunk)
                (run / "serial.log").write_bytes(data)
            else:
                time.sleep(0.02)
        raise RuntimeError(f"guest did not produce {marker!r}; serial log: {run/'serial.log'}")

    def send_line(line):
        guest.stdin.write(line.encode() + b"\n")
        guest.stdin.flush()

    try:
        read_until(b"LogitOS shell", time.monotonic() + 180)
        cases = [
            "/bin/sh -c '/bin/echo FIRST_ON_DISK > /sh_list_a; /bin/echo SECOND_ON_DISK > /sh_list_b; /bin/echo \"SEMI;LITERAL\" > /sh_list_quote'",
            "/bin/sh -c 'export X=one; /bin/echo $X > /sh_list_var'",
            "/bin/sh -c '/bin/false; /bin/echo $? > /sh_list_status'",
        ]
        for number, line in enumerate(cases, 1):
            marker = f"SH_COMMANDS_DONE_{number}".encode()
            send_line(line)
            # The split spelling cannot appear in the tty's echo of this input.
            # Retrying only the harmless marker prevents a diagnostic inserted
            # into stdout from making the original command run twice.
            marker_line = f'echo "SH_COMMANDS_"DONE_{number}'
            send_line(marker_line)
            deadline = time.monotonic() + 90
            while marker not in data and time.monotonic() < deadline:
                try:
                    read_until(marker, min(deadline, time.monotonic() + 8))
                except RuntimeError:
                    if guest.poll() is not None or time.monotonic() >= deadline:
                        raise
                    send_line(marker_line)
        if b"LOGIT_PANIC" in data or b"EXCEPTION" in data:
            raise AssertionError("guest panicked during command-list test")
    finally:
        if guest.poll() is None:
            guest.terminate()
        try:
            guest.wait(timeout=10)
        except subprocess.TimeoutExpired:
            guest.kill()
            guest.wait()
        guest.stdin.close()
        guest.stdout.close()
        (run / "serial.log").write_bytes(data)

    expected = {
        "/sh_list_a": b"FIRST_ON_DISK\n",
        "/sh_list_b": b"SECOND_ON_DISK\n",
        "/sh_list_quote": b"SEMI;LITERAL\n",
        "/sh_list_var": b"one\n",
        "/sh_list_status": b"1\n",
    }
    fs = LogitFS(disk)
    try:
        for path, value in expected.items():
            inode = fs.lookup(path)
            if inode is None:
                raise AssertionError(f"guest did not create {path}; serial log: {run/'serial.log'}")
            actual = fs.read_file(inode)
            if actual != value:
                raise AssertionError(f"{path}: got {actual!r}, expected {value!r}")
    finally:
        fs.close()
    if digest((build / "sh.aex").read_bytes()) != source_hash:
        raise AssertionError("source shell artifact changed during guest test")
    print(f"PASS: sh -c sequential commands wrote {len(expected)} exact guest files; evidence {run}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
