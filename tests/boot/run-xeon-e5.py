#!/usr/bin/env python3
"""Boot one high-thread-count x86 profile and verify serial evidence."""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import socket
import subprocess
import tempfile
import threading
import time


def wait_for(log, process, markers, timeout):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and process.poll() is None:
        data = bytes(log)
        if any(marker in data for marker in markers):
            return
        time.sleep(0.1)
    raise RuntimeError("timeout waiting for " + repr(markers))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--firmware", choices=("bios", "uefi"), required=True)
    parser.add_argument("--cpu", required=True)
    parser.add_argument("--smp", type=int, required=True)
    parser.add_argument("--model", type=lambda s: int(s, 0), required=True)
    parser.add_argument("--generation", required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=420)
    parser.add_argument("--expect-cap-failure", action="store_true")
    parser.add_argument("--expect-osxsave-failure", action="store_true")
    args = parser.parse_args()
    if args.smp < 1 or args.smp > 32 or args.smp % 2:
        parser.error("--smp must be an even count from 2 through 32")

    args.out.mkdir(parents=True, exist_ok=True)
    serial_log = args.out / "serial.log"
    qemu_log = args.out / "qemu.log"
    log = bytearray()

    with tempfile.TemporaryDirectory(prefix="logit-xeon-e5-") as temp:
        serial_path = str(Path(temp) / "serial.sock")
        command = [
            os.environ.get("QEMU", "qemu-system-x86_64"),
            "-cpu", args.cpu,
            "-smp", f"{args.smp},sockets=1,cores={args.smp // 2},threads=2",
            "-m", "16G",
            "-accel", "tcg,thread=multi",
            "-display", "none",
            "-vga", "none",
            "-device", "virtio-gpu-pci,xres=1280,yres=800",
            "-netdev", "user,id=n0",
            "-device", "e1000,netdev=n0",
            "-drive", f"file={args.disk.resolve()},format=raw,if=none,id=hd0,file.locking=off",
            "-device", "virtio-blk-pci,drive=hd0",
            "-chardev", f"socket,id=ser0,path={serial_path},server=on,wait=on",
            "-serial", "chardev:ser0",
            "-monitor", "none",
            "-no-reboot",
            "-snapshot",
        ]
        if args.firmware == "bios":
            command += [
                "-machine", "pc",
                "-cdrom", str((args.build / "logit.iso").resolve()),
                "-boot", "d",
            ]
        else:
            code = Path(os.environ.get(
                "OVMF_CODE", "/opt/homebrew/share/qemu/edk2-x86_64-code.fd"))
            vars_source = Path(os.environ.get(
                "OVMF_VARS_SRC", "/opt/homebrew/share/qemu/edk2-i386-vars.fd"))
            vars_copy = Path(temp) / "vars.fd"
            shutil.copyfile(vars_source, vars_copy)
            command += [
                "-machine", "q35",
                "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
                "-drive", f"if=pflash,format=raw,file={vars_copy}",
                "-device", "ich9-ahci,id=ahci0",
                "-drive", f"file={(args.build / 'esp.img').resolve()},format=raw,if=none,id=esp0,file.locking=off",
                "-device", "ide-hd,drive=esp0,bus=ahci0.0",
            ]

        (args.out / "command.json").write_text(
            json.dumps(command, indent=2) + "\n")
        qerr = qemu_log.open("wb")
        process = subprocess.Popen(
            command, stdout=qerr, stderr=subprocess.STDOUT)
        serial = None
        try:
            serial = socket.socket(socket.AF_UNIX)
            for _ in range(600):
                try:
                    serial.connect(serial_path)
                    break
                except OSError:
                    if process.poll() is not None:
                        raise RuntimeError("QEMU exited before serial connected")
                    time.sleep(0.1)
            else:
                raise RuntimeError("serial socket did not appear")

            def read_serial():
                try:
                    while True:
                        chunk = serial.recv(65536)
                        if not chunk:
                            break
                        log.extend(chunk)
                        serial_log.write_bytes(log)
                except OSError:
                    pass

            threading.Thread(target=read_serial, daemon=True).start()
            wait_for(log, process, (b"LogitOS shell",), args.timeout)
            serial.sendall(f"/bin/xeon-e5-check {args.smp}\n".encode())
            wait_for(
                log, process,
                (b"XEON_E5_GUEST_OK", b"XEON_E5_GUEST_FAIL"),
                args.timeout)
            text = bytes(log).decode(errors="replace")

            evidence = {
                "boot": "LOGIT_BOOT_OK" in text,
                "platform": bool(re.search(
                    rf"\[cpu-platform\] family=6 model=0x{args.model:x} "
                    rf"stepping=\d+ generation={re.escape(args.generation)}\b",
                    text)),
                "topology": bool(re.search(
                    rf"\[cpu\] topology: .*cpuid-addressable/package={args.smp} "
                    rf"cores/package={args.smp // 2} threads/core=2\b", text)),
                "mca": "[cpu] mca: banks=" in text,
                "simd_interrupts": "CPU_SIMD_SELFTEST_OK" in text,
                "no_panic": "*** LOGIT PANIC" not in text,
            }
            if args.expect_cap_failure:
                evidence.update({
                    "cap_truncated": f"[smp] 8/{args.smp} CPUs online" in text,
                    "runtime_count_is_8": (
                        f"XEON_E5_GUEST_FAIL online=8 expected={args.smp}" in text),
                    "positive_absent": "XEON_E5_GUEST_OK" not in text,
                })
            elif args.expect_osxsave_failure:
                evidence.update({
                    "osxsave_inherited": bool(re.search(
                        r"\[cpu\] xstate: avx_hw=1 avx_os=0 osxsave=1\b", text)),
                    "smp_online": (
                        f"[smp] {args.smp}/{args.smp} CPUs online" in text),
                    "ring3": "XEON_E5_GUEST_OK" in text,
                })
            else:
                expected_workers = min(args.smp, 12)
                evidence.update({
                    "osxsave_cleared": bool(re.search(
                        r"\[cpu\] xstate: avx_hw=1 avx_os=0 osxsave=0\b", text)),
                    "smp_online": (
                        f"[smp] {args.smp}/{args.smp} CPUs online" in text),
                    "runtime_count": bool(re.search(
                        rf"XEON_E5_GUEST online={args.smp} expected={args.smp} "
                        rf"workers={expected_workers}\b", text)),
                    "ring3": "XEON_E5_GUEST_OK" in text,
                    "guest_no_failure": "XEON_E5_GUEST_FAIL" not in text,
                })
            (args.out / "result.json").write_text(
                json.dumps(evidence, indent=2, sort_keys=True) + "\n")
            failed = [name for name, value in evidence.items() if not value]
            if failed:
                raise RuntimeError("missing evidence: " + ", ".join(failed))
            label = "cap-negctl" if args.expect_cap_failure else (
                "osxsave-negctl" if args.expect_osxsave_failure else "positive")
            print(f"PASS {args.firmware} {args.smp}T {label}")
        finally:
            if process.poll() is None:
                process.terminate()
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            if serial is not None:
                serial.close()
            qerr.close()
            serial_log.write_bytes(log)


if __name__ == "__main__":
    main()
