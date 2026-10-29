#!/usr/bin/env python3
"""Boot a 28-thread, sparse-x2APIC machine through BIOS and UEFI.

The QEMU topology exercises MADT enumeration, full-width APIC destinations,
INIT-SIPI-SIPI startup, a fixed IPI to every AP, and vector-32 delivery on all
online CPUs.  QEMU does not emulate Raptor Lake P/E CPUID.1A behavior, so the
8P/12E/20-core aggregation is proved by the paired host fixture instead.
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
LOW_IDS = [0, 1, 4, 5, 8, 9, 12, 13, 16, 17, 20, 21, 24, 25, 28, 29]
WIDE_IDS = list(range(0x100, 0x10C))
APIC_IDS = LOW_IDS + WIDE_IDS
EXPECTED_MASK = (1 << len(APIC_IDS)) - 1


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


def topology_evidence(text):
    madt = re.search(
        r"\[acpi\] MADT CPUs type0=(\d+) type9=(\d+) duplicate=(\d+) rejected=(\d+)",
        text,
    )
    topology = re.search(
        r"\[smp\] topology logical=(\d+) cores=(\d+) "
        r"p=(\d+)/(\d+) e=(\d+)/(\d+) unknown=(\d+)/(\d+) "
        r"mismatch=(\d+) conflict=(\d+)",
        text,
    )
    online_ids = {
        int(value)
        for value in re.findall(r"\[smp\] CPU \d+ apic_id=(\d+) online", text)
    }
    cpuid_topology_ids = {
        int(value)
        for value in re.findall(
            r"\[smp\] CPU \d+ apic_id=(\d+) topology=cpuid\.(?:1f|0b) ",
            text,
        )
    }
    # The BSP has a topology line but no separate "online" line.
    online_ids.add(0) if "[smp] CPU 0 apic_id=0 topology=" in text else None
    return madt, topology, online_ids, cpuid_topology_ids


def required_markers_present(text):
    return (
        "LOGIT_BOOT_OK" in text
        and "[smp] 28/28 CPUs online" in text
        and "[raptor-smp] fixed IPI APs 27/27 observed; BSP sender" in text
        and f"[raptor-smp] timer vector32 all-online mask={EXPECTED_MASK:x}" in text
    )


def fatal_marker_present(text):
    return "*** LOGIT PANIC" in text or "*** EXCEPTION:" in text


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

    # APIC IDs below 255 and above 255 deliberately coexist. maxcpus reserves
    # the sparse topology without creating hundreds of running vCPUs.
    common = [
        QEMU,
        "-machine", "q35",
        "-cpu", "max,+x2apic",
        "-smp", "1,maxcpus=288,sockets=288,cores=1,threads=1",
    ]
    for apic_id in APIC_IDS[1:]:
        common += [
            "-device", f"max-x86_64-cpu,id=cpu{apic_id},apic-id={apic_id}"
        ]
    common += [
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
        timeout = float(os.environ.get("RAPTOR_SMP_BOOT_TIMEOUT", "600"))
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = read(serial)
            if required_markers_present(text) or fatal_marker_present(text):
                break
            if proc.poll() is not None:
                raise RuntimeError("QEMU exited before all SMP evidence markers")
            time.sleep(0.2)

        text = read(serial)
        madt, topology, online_ids, cpuid_topology_ids = topology_evidence(text)
        madt_type0 = int(madt.group(1)) if madt else -1
        madt_type9 = int(madt.group(2)) if madt else -1
        wide_aliases = re.findall(
            r"MADT/LAPIC apic_id=\d+ != CPUID apic_id=\d+; topology isolated",
            text,
        )
        evidence = {
            "scope": (
                "synthetic MADT type-0/type-9 record-format coverage; these "
                "record types do not identify P-cores or E-cores"
            ),
            "boot": "LOGIT_BOOT_OK" in text,
            "madt_type0_count": madt_type0,
            "madt_type9_count": madt_type9,
            "madt_28_enabled": madt_type0 + madt_type9 == 28,
            "madt_type0_present": madt_type0 > 0,
            "madt_type9_present": madt_type9 > 0,
            "madt_clean": bool(
                madt and int(madt.group(3)) == 0 and int(madt.group(4)) == 0
            ),
            "x2_msr_backend": bool(re.search(
                r"\[lapic\] mode=x2apic id=\d+ source=IA32_APIC_BASE TEST-ONLY-force",
                text,
            )),
            "all_expected_apic_ids_online": online_ids == set(APIC_IDS),
            "twenty_eight_online": "[smp] 28/28 CPUs online" in text,
            "topology_28_logical": bool(
                topology and int(topology.group(1)) == 28
            ),
            # The current TCG model supplies full IDs through CPUID.0B. Keep
            # the observed alias count in the artifact so a fallback to the
            # eight-bit CPUID.1 identity cannot be reported as a clean run.
            "wide_cpuid_alias_count": len(wide_aliases),
            "wide_cpuid_identity_clean": bool(
                topology and int(topology.group(9)) == 0
                and int(topology.group(10)) == 0
                and set(WIDE_IDS).issubset(cpuid_topology_ids)
                and len(wide_aliases) == 0
            ),
            "all_expected_extended_topology_ids":
                cpuid_topology_ids == set(APIC_IDS),
            "uniform_scheduler_policy":
                "[smp] scheduler topology policy=uniform-online-cpus" in text,
            "fixed_ipi_all_aps":
                "[raptor-smp] fixed IPI APs 27/27 observed; BSP sender" in text,
            "fixed_ipi_bsp_role": "sender",
            "timer_all":
                f"[raptor-smp] timer vector32 all-online mask={EXPECTED_MASK:x}" in text,
            "ioapic_device_routes":
                "[ioapic] device IRQs routed via I/O APIC" in text,
            "no_ap_timeout": "stack quarantined" not in text,
            "no_ipi_failure": "[raptor-smp] fixed IPI FAILED" not in text,
            "no_unsafe_route": "unsafe partial legacy route" not in text
                and "initial mask state unsafe" not in text,
            "no_exception_or_panic": not fatal_marker_present(text),
        }
        if kind == "uefi":
            evidence["uefi_handoff"] = all(
                marker in text for marker in
                ("[efi] gop ", "[efi] ebs ok", "[efi] jump")
            )
        (run / "result.json").write_text(
            json.dumps(evidence, indent=2, sort_keys=True) + "\n"
        )
        failed = [
            key for key, value in evidence.items()
            if isinstance(value, bool) and not value
        ]
        if failed:
            raise RuntimeError("missing evidence: " + ", ".join(failed))
        print(
            f"PASS {kind}: 28 CPUs, mixed MADT type0/type9, sparse x2APIC, "
            "all-AP fixed IPI and all-CPU timer"
        )
        return evidence
    except Exception:
        print(f"--- {kind} serial tail ---")
        print(read(serial)[-24000:])
        print(f"--- {kind} QEMU stderr ---")
        print(read(stderr)[-8000:])
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
        "scope": (
            "QEMU synthetic 28-CPU sparse-x2APIC execution. MADT type 0/9 "
            "are firmware record formats, not P/E core classes. P/E CPUID "
            "and physical i7-14700KF remain unverified"
        ),
        "apic_ids": APIC_IDS,
        "bios": run_one("bios", iso, disk, out),
        "uefi": run_one("uefi", esp, disk, out, code, vars_source),
    }
    (out / "summary.json").write_text(json.dumps(results, indent=2) + "\n")
    print(
        "RAPTOR_SMP_GUEST: BIOS + UEFI 28-CPU sparse-ID paths passed; "
        "27 AP fixed-IPI receivers plus all-CPU timers passed; hybrid CPUID "
        "and physical hardware unverified"
    )


if __name__ == "__main__":
    main()
