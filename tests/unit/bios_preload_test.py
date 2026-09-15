#!/usr/bin/env python3
"""Boot and validate the narrow BIOS preload -> loader handoff."""

import argparse
import os
from pathlib import Path
import struct
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
import mkiso  # noqa: E402 -- the writer is the patch-format authority

FIRST_MARKER = "LOGIT_BIOS_PRELOAD_OK"
SECOND_MARKER = "LOGIT_BIOS_LOADER_OK"


def fail(message):
    print(f"FAIL: {message}")
    return 1


def boot_image_offset(image):
    boot_record = mkiso.ISO_SECTOR * mkiso.BOOT_RECORD_LBA
    if image[boot_record:boot_record + 7] != b"\x00CD001\x01":
        raise ValueError("El Torito boot record is absent at descriptor 17")
    catalog_lba = struct.unpack_from("<I", image, boot_record + 71)[0]
    catalog = catalog_lba * mkiso.ISO_SECTOR
    if image[catalog + 32] != 0x88:
        raise ValueError("El Torito initial entry is not bootable")
    return struct.unpack_from("<I", image, catalog + 40)[0] * mkiso.ISO_SECTOR


def check_native_sector_mapping(iso_path, loader_path):
    with open(iso_path, "rb") as source:
        image = source.read()
    with open(loader_path, "rb") as source:
        loader = source.read()

    boot = boot_image_offset(image)
    patch = image.find(mkiso.LOADER_PATCH_MAGIC, boot, boot + mkiso.ISO_SECTOR)
    if patch < 0:
        return fail("preload patch magic is absent from the catalog-loaded image")
    _, loader_lba, blocks = mkiso.LOADER_PATCH.unpack_from(image, patch)
    native_offset = loader_lba * mkiso.ISO_SECTOR
    if image[native_offset:native_offset + len(loader)] != loader:
        return fail(f"patched native-CD LBA {loader_lba} does not contain the loader fixture")

    legacy_offset = loader_lba * 512
    if image[legacy_offset:legacy_offset + len(loader)] == loader:
        return fail("512-byte and 2,048-byte LBA interpretations reached the same fixture bytes")
    print(f"PASS: patched LBA {loader_lba} contains {len(loader)} controlled bytes at "
          f"{loader_lba} * 2048; {loader_lba} * 512 does not")
    print(f"PASS: preload requests {blocks} native 2048-byte block(s)")
    return 0


def run_qemu(qemu, iso_path, timeout):
    command = [
        qemu, "-accel", "tcg", "-machine", "pc", "-m", "32M",
        "-display", "none", "-monitor", "none", "-serial", "stdio",
        "-no-reboot", "-no-shutdown", "-boot", "d", "-cdrom", iso_path,
    ]
    try:
        process = subprocess.Popen(command, stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT, text=True)
    except FileNotFoundError:
        print(f"SKIP: test-bios-preload requires {qemu}; install QEMU to settle the guest path")
        return None
    try:
        output, _ = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        process.kill()
        output, _ = process.communicate()
    return output.replace("\r", "")


def print_relevant_serial(output):
    relevant = [line for line in output.splitlines()
                if "LOGIT_BIOS_" in line or "LOGIT BIOS:" in line]
    print("QEMU serial: " + (" | ".join(relevant) if relevant else "<no LogitOS marker>"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("iso")
    parser.add_argument("--loader")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--qemu-only", action="store_true")
    parser.add_argument("--catalog-probe-sectors", type=int)
    parser.add_argument("--timeout", type=float, default=3.0)
    args = parser.parse_args()

    if not os.path.isfile(args.iso):
        return fail(f"ISO does not exist: {args.iso}")
    if args.catalog_probe_sectors is None and not args.loader:
        return fail("--loader is required for the handoff check")
    if not args.qemu_only and args.loader:
        rc = check_native_sector_mapping(args.iso, args.loader)
        if rc:
            return rc

    output = run_qemu(args.qemu, args.iso, args.timeout)
    if output is None:
        return 0
    print_relevant_serial(output)

    if args.catalog_probe_sectors is not None:
        marker = f"LOGIT_BIOS_CATALOG_{args.catalog_probe_sectors}_OK"
        if marker not in output:
            return fail(f"SeaBIOS did not deliver the requested {args.catalog_probe_sectors} "
                        "catalog sectors through the final controlled sentinel")
        print(f"PASS: SeaBIOS delivered at least {args.catalog_probe_sectors} x 512 bytes "
              f"({args.catalog_probe_sectors * 512} bytes) from the catalog preload")
        return 0

    first = output.find(FIRST_MARKER)
    second = output.find(SECOND_MARKER)
    if first < 0:
        return fail(f"QEMU did not observe {FIRST_MARKER}")
    if second < 0:
        return fail(f"QEMU observed {FIRST_MARKER} but not {SECOND_MARKER}")
    if second < first:
        return fail("QEMU observed the loader marker before the preload marker")
    print("PASS: QEMU observed the preload marker followed by the loader marker")
    return 0


if __name__ == "__main__":
    sys.exit(main())
