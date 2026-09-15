#!/usr/bin/env python3
"""Parse mkiso output and optionally prove firmware transfers control to it."""

import argparse
import os
import struct
import subprocess
import sys


SECTOR = 2048
MARKER = "LOGIT_MKISO_FIXTURE_OK"


def fail(message):
    print(f"FAIL: {message}")
    return False


def check_equal(label, actual, expected):
    if actual != expected:
        return fail(f"{label}: got {actual!r}, expected {expected!r}")
    return True


def parse_image(path):
    data = open(path, "rb").read()
    if len(data) < 218 * SECTOR:
        return fail(f"image length: got {len(data)}, need at least {218 * SECTOR}")

    pvd = data[16 * SECTOR:17 * SECTOR]
    boot_record = data[17 * SECTOR:18 * SECTOR]
    terminator = data[18 * SECTOR:19 * SECTOR]
    catalog = data[49 * SECTOR:50 * SECTOR]

    checks = [
        check_equal("PVD type", pvd[0], 1),
        check_equal("PVD identifier", pvd[1:6], b"CD001"),
        check_equal("PVD version", pvd[6], 1),
        check_equal("boot record type", boot_record[0], 0),
        check_equal("boot record identifier", boot_record[1:6], b"CD001"),
        check_equal("boot record version", boot_record[6], 1),
        check_equal("boot system identifier", boot_record[7:39],
                    b"EL TORITO SPECIFICATION".ljust(32, b"\x00")),
        check_equal("boot catalog LBA", struct.unpack_from("<I", boot_record, 71)[0], 49),
        check_equal("terminator type", terminator[0], 255),
        check_equal("terminator identifier", terminator[1:6], b"CD001"),
        check_equal("terminator version", terminator[6], 1),
        check_equal("catalog validation header id", catalog[0], 1),
        check_equal("catalog validation platform id", catalog[1], 0),
        check_equal("catalog validation key", catalog[30:32], b"\x55\xaa"),
        check_equal("catalog validation 16-bit sum",
                    sum(struct.unpack("<16H", catalog[:32])) & 0xFFFF, 0),
        check_equal("initial entry boot indicator", catalog[32], 0x88),
        check_equal("initial entry media type", catalog[33], 0),
        check_equal("initial entry load segment", struct.unpack_from("<H", catalog, 34)[0],
                    0x07C0),
        check_equal("initial entry sector count", struct.unpack_from("<H", catalog, 38)[0], 4),
        check_equal("initial entry image LBA", struct.unpack_from("<I", catalog, 40)[0], 217),
    ]
    if not all(checks):
        return False
    print("PASS: mkiso host layout: PVD/boot record/terminator, catalog validation, and initial entry")
    return True


def qemu_boot(path, qemu, timeout):
    command = [
        qemu, "-machine", "pc", "-m", "16M", "-display", "none",
        "-monitor", "none", "-serial", "stdio", "-no-reboot",
        "-boot", "order=d", "-cdrom", path,
    ]
    output = ""
    try:
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                timeout=timeout, check=False)
        output = result.stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired as error:
        captured = error.stdout or b""
        if isinstance(captured, str):
            output = captured
        else:
            output = captured.decode("utf-8", errors="replace")
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    if MARKER not in output:
        return fail(f"QEMU did not observe {MARKER} before {timeout:g}s timeout")
    print(f"PASS: QEMU observed {MARKER}; BIOS accepted the catalog and transferred control")
    return True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("iso")
    parser.add_argument("--qemu", help="also boot with this qemu-system-x86_64 binary")
    parser.add_argument("--qemu-only", action="store_true",
                        help="skip host parsing (used to test intentionally corrupt catalogs)")
    parser.add_argument("--timeout", type=float, default=4.0)
    args = parser.parse_args()

    if not os.path.isfile(args.iso):
        sys.exit(f"FAIL: no such ISO: {args.iso}")
    ok = True
    if not args.qemu_only:
        ok = parse_image(args.iso)
    if args.qemu:
        ok = qemu_boot(args.iso, args.qemu, args.timeout) and ok
    elif args.qemu_only:
        sys.exit("FAIL: --qemu-only requires --qemu")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
