#!/usr/bin/env python3
"""Boot the production kernel with QEMU's HPET present and absent.

This proves only the emulated ACPI/MMIO path.  The X79 machine still needs its
own serial log because QEMU cannot reproduce that board's firmware tables.
"""
import argparse
import pathlib
import subprocess
import tempfile
import time


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--iso", required=True)
    ap.add_argument("--enabled", choices=("yes", "no"), required=True)
    args = ap.parse_args()
    iso = pathlib.Path(args.iso).resolve()
    enabled = args.enabled == "yes"
    with tempfile.TemporaryDirectory(prefix="logitos-hpet-") as td:
        serial = pathlib.Path(td) / "serial.txt"
        cmd = [
            "qemu-system-x86_64", "-accel", "tcg,thread=multi", "-smp", "4", "-m", "512",
            # Hide CPUID's hypervisor bit as well as qemu64's absent invariant
            # TSC.  That enters the same policy branch as old bare metal and
            # proves HPET really replaces PIT instead of only registering.
            "-cpu", "qemu64,-hypervisor", "-machine", f"pc,hpet={'on' if enabled else 'off'}",
            "-cdrom", str(iso), "-display", "none", "-monitor", "none",
            "-serial", f"file:{serial}", "-no-reboot", "-no-shutdown",
        ]
        proc = subprocess.Popen(cmd)
        text = ""
        try:
            deadline = time.monotonic() + (35 if enabled else 15)
            while time.monotonic() < deadline:
                if serial.exists():
                    text = serial.read_text(errors="replace")
                    if (enabled and "[time] fallback pit ran" in text
                            and "[time] smp-mono" in text):
                        break
                    if not enabled and "[hpet] unavailable: ACPI HPET table absent" in text:
                        break
                if proc.poll() is not None:
                    break
                time.sleep(0.05)
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        if enabled:
            xcheck_ok = any(
                "[time] xcheck src=hpet" in line
                and " OK" in line
                and " FAIL" not in line
                for line in text.splitlines()
            )
            smp_ok = any(
                "[time] smp-mono cores=4 seen=f reads=400000" in line
                and "observed-backsteps=0" in line
                for line in text.splitlines()
            )
            ok = (
                "[hpet] ready:" in text
                and "[time] HPET active" in text
                and xcheck_ok
                and "fallback pit ran" in text
                and "switch back restore=hpet rc=0" in text
                and "monotonic" in text.split("fallback pit ran", 1)[1]
                and smp_ok
            )
            label = "present -> active, xchecked, PIT restored HPET, 4-core monotonic"
        else:
            ok = "[hpet] ready:" not in text and "[hpet] unavailable: ACPI HPET table absent" in text
            label = "absent -> refused"
        if not ok:
            print(text[-5000:])
            print(f"HPET-GUEST-FAIL: {label}")
            return 1
        print(f"HPET-GUEST-OK: {label}")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
