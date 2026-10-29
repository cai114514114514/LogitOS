#!/usr/bin/env python3
"""Boot a sparse x2APIC topology through BIOS and UEFI under QEMU.

This is synthetic execution evidence. CPU 1 is explicitly assigned APIC ID
256, which forces a MADT type-9 entry and exercises a wide INIT/SIPI target.
It does not establish behavior on a physical X79 board.
"""
from pathlib import Path
import argparse
import json
import os
import re
import shutil
import subprocess
import time


QEMU = os.environ.get("QEMU", "qemu-system-x86_64")


def read(path):
    try:
        return path.read_text(errors="replace")
    except OSError:
        return ""


def firmware_path(env, candidates):
    configured = os.environ.get(env)
    if configured:
        path = Path(configured)
        return path if path.is_file() else None
    for candidate in candidates:
        path = Path(candidate)
        if path.is_file():
            return path
    return None


def run_one(kind, boot_image, disk, out, code=None, vars_source=None):
    run = out / kind
    run.mkdir(parents=True, exist_ok=True)
    serial = run / "serial.log"
    stderr = run / "qemu.stderr.log"
    for stale in (serial, stderr, run / "result.json", run / "command.json"):
        try:
            stale.unlink()
        except FileNotFoundError:
            pass

    common = [
        QEMU,
        "-machine", "q35",
        "-cpu", "max,+x2apic",
        # One ordinary BSP plus one statically plugged CPU at APIC ID 256.
        # maxcpus/topology makes that sparse ID valid without running 257 CPUs.
        "-smp", "1,maxcpus=257,sockets=257,cores=1,threads=1",
        "-device", "max-x86_64-cpu,id=cpu256,apic-id=256",
        "-device", "intel-iommu,intremap=on,eim=on",
        "-m", "1G",
        "-accel", "tcg,thread=multi",
        "-display", "none",
        "-vga", "none",
        "-device", "virtio-gpu-pci,xres=1280,yres=800",
        "-net", "none",
        "-drive", f"file={disk},format=raw,if=none,id=root,file.locking=off",
        "-device", "virtio-blk-pci,drive=root",
        "-serial", "file:" + str(serial),
        "-monitor", "none",
        "-no-reboot",
        "-snapshot",
    ]
    if kind == "bios":
        command = common + ["-cdrom", str(boot_image), "-boot", "d"]
    else:
        vars_copy = run / "OVMF_VARS.fd"
        shutil.copyfile(vars_source, vars_copy)
        command = common + [
            "-drive", f"if=pflash,format=raw,readonly=on,file={code}",
            "-drive", f"if=pflash,format=raw,file={vars_copy}",
            "-device", "ich9-ahci,id=ahci0",
            "-drive", f"file={boot_image},format=raw,if=none,id=esp,file.locking=off",
            "-device", "ide-hd,drive=esp,bus=ahci0.0",
            "-boot", "order=c,menu=off",
        ]
    (run / "command.json").write_text(json.dumps(command, indent=2) + "\n")

    with stderr.open("wb") as err:
        proc = subprocess.Popen(command, stdout=subprocess.DEVNULL, stderr=err)
    try:
        deadline = time.monotonic() + float(os.environ.get("X2APIC_BOOT_TIMEOUT", "360"))
        while time.monotonic() < deadline:
            text = read(serial)
            if "LOGIT_BOOT_OK" in text or "*** LOGIT PANIC" in text:
                break
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited before the boot marker")
            time.sleep(0.2)
        text = read(serial)
        madt = re.search(
            r"\[acpi\] MADT CPUs type0=(\d+) type9=(\d+) duplicate=(\d+) rejected=(\d+)",
            text)
        evidence = {
            "boot": "LOGIT_BOOT_OK" in text,
            "type9_present": bool(madt and int(madt.group(2)) > 0),
            "x2_msr_backend": bool(re.search(
                r"\[lapic\] mode=x2apic id=\d+ source=IA32_APIC_BASE TEST-ONLY-force", text)),
            "wide_ap_online": "[smp] CPU 1 apic_id=256 online" in text,
            "two_online": "[smp] 2/2 CPUs online" in text,
            "ioapic_device_routes":
                "[ioapic] device IRQs routed via I/O APIC" in text,
            "no_ap_timeout": "stack quarantined" not in text,
            "no_unsafe_route": "unsafe partial legacy route" not in text and
                               "initial mask state unsafe" not in text,
            "no_panic": "*** LOGIT PANIC" not in text,
        }
        if kind == "uefi":
            evidence["uefi_handoff"] = all(marker in text for marker in
                                            ("[efi] gop ", "[efi] ebs ok", "[efi] jump"))
        (run / "result.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n")
        failed = [key for key, value in evidence.items() if not value]
        if failed:
            raise RuntimeError("missing evidence: " + ", ".join(failed))
        print(f"PASS {kind}: MADT type9 + x2APIC MSR + APIC-ID-256 AP online")
        return evidence
    except Exception:
        print(f"--- {kind} serial tail ---")
        print(read(serial)[-16000:])
        print(f"--- {kind} QEMU stderr ---")
        print(read(stderr)[-4000:])
        raise
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, required=True)
    parser.add_argument("--esp", type=Path, required=True)
    parser.add_argument("--disk", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    iso, esp, disk = (args.iso.resolve(), args.esp.resolve(), args.disk.resolve())
    for path in (iso, esp, disk):
        if not path.is_file():
            parser.error("missing image: " + str(path))
    if not shutil.which(QEMU):
        parser.error("QEMU not found: " + QEMU)
    code = firmware_path("OVMF_CODE", [
        "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
        "/usr/share/OVMF/OVMF_CODE_4M.fd",
    ])
    vars_source = firmware_path("OVMF_VARS_SRC", [
        "/opt/homebrew/share/qemu/edk2-i386-vars.fd",
        "/usr/share/OVMF/OVMF_VARS_4M.fd",
    ])
    if not code or not vars_source:
        parser.error("OVMF unavailable; set OVMF_CODE and OVMF_VARS_SRC")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    results = {
        "scope": "QEMU synthetic sparse x2APIC execution; not physical X79 evidence",
        "bios": run_one("bios", iso, disk, out),
        "uefi": run_one("uefi", esp, disk, out, code, vars_source),
    }
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print("X2APIC_GUEST: BIOS + UEFI synthetic sparse-ID paths passed; physical X79 unverified")


if __name__ == "__main__":
    main()
