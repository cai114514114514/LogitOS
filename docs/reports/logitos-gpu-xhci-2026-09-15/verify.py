#!/usr/bin/env python3
"""Verify artifact-bound xHCI and GTX 1050 bring-up evidence."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


REPORT = Path(__file__).resolve().parent
ROOT = REPORT.parents[1]
EVIDENCE = REPORT / "evidence"

EXPECTED = {
    ROOT / "build-gpu-xhci-final/logit.iso":
        "4c7f651fea2dd10b19ff4fe72bbd4ca2ae17dcbd0dbbc1ed934c590f7d91403e",
    ROOT / "build-gpu-xhci-final/disk.img":
        "7a364ff778e8dcab65f4220988e5c8b86c209653133c26a8731d18acd9495a02",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require_markers(path: Path, markers: tuple[str, ...]) -> str:
    require(path.is_file(), f"missing evidence: {path}")
    text = path.read_text(errors="replace")
    for marker in markers:
        require(marker in text, f"missing marker {marker!r} in {path}")
    return text


def main() -> None:
    for path, expected in EXPECTED.items():
        require(path.is_file(), f"missing artifact: {path}")
        require(sha256(path) == expected, f"artifact hash changed: {path}")

    usb = json.loads((EVIDENCE / "usb-hotplug-result.json").read_text())
    require(isinstance(usb, list) and len(usb) == 2, "USB result must contain two controllers")
    require({row.get("controller") for row in usb} == {"xhci", "ehci"},
            "USB result is not exactly xHCI plus EHCI")
    iso_hash, disk_hash = EXPECTED.values()
    for row in usb:
        name = row["controller"]
        require(row.get("qemu_accel") == "tcg", f"{name} did not use TCG")
        require(row.get("post_boot_attach") is True, f"{name} was not attached after boot")
        require(row.get("scsi_capacity_and_mbr_read") is True, f"{name} did not read MBR")
        require(row.get("partition_published") == "usb0p1", f"{name} did not publish usb0p1")
        require(row.get("disconnect_removed_devices") == 1, f"{name} remove count changed")
        require(row.get("block_class_offline") is True, f"{name} block class stayed online")
        require(row.get("iso_sha256") == iso_hash, f"{name} ISO hash is unbound")
        require(row.get("disk_sha256") == disk_hash, f"{name} disk hash is unbound")

    gpu_log = require_markers(EVIDENCE / "test-nvidia-pascal-host.log", (
        "NV_ACCEL_NEGCTL: missing firmware reached BAR map (exactly one failure)",
        "NV_ACCEL_NEGCTL: corrupt firmware reached BAR map (exactly one failure)",
        "NV_ACCEL_BRINGUP: 38 checks, 0 failures",
        "NV_BOOTFB: 56 checks, 0 failures",
    ))
    require("NV_BOOTFB_NEGCTL" in gpu_log, "passive GPU negative controls are absent")

    life_log = require_markers(EVIDENCE / "test-xhci-lifecycle.log", (
        "xHCI lifecycle: 18 checks, 0 failures",
        "EXPECTED-FAIL xHCI EVENT_RING_READBACK",
        "EXPECTED-FAIL xHCI RUN_HCE",
        "EXPECTED-FAIL xHCI RESTORE_FAILURE",
    ))
    require(life_log.count("EXPECTED-FAIL xHCI ") == 9,
            "xHCI lifecycle negative-control count changed")
    require_markers(EVIDENCE / "test-xhci-xfer.log", (
        "positive: xHCI transfers: 21 checks, 0 failures",
    ))

    for firmware, dimensions in (("bios", (1024, 768)), ("uefi", (1280, 800))):
        result = json.loads((EVIDENCE / f"nvidia-synthetic-{firmware}/result.json").read_text())
        require(result.get("evidence") == "synthetic-qemu-stdvga",
                f"{firmware} display evidence is not synthetic stdvga")
        require((result.get("width"), result.get("height")) == dimensions,
                f"{firmware} display dimensions changed")
        require(result.get("physical_gtx1050_verified") is False,
                f"{firmware} result overclaims physical GTX 1050 proof")

    kernel_map = require_markers(EVIDENCE / "kernel-map-nvidia.txt", (
        "nvidia_pascal_accel.o:(.text)",
        "nvidia_pascal_accel_prepare",
        "nvidia_pascal_accel_fill",
        "nvidia_pascal_accel_copy",
        "nvidia_pascal_accel_query",
    ))
    require(kernel_map.count("nvidia_pascal_accel.o") >= 4,
            "Pascal bring-up object is not represented in the final kernel map")

    accel = (ROOT / "c/drivers/gpu/nvidia_pascal_accel.c").read_text()
    header = (ROOT / "c/drivers/gpu/nvidia_pascal_accel.h").read_text()
    rows = re.findall(r'\{ "([^"]+)", (\d+), "([0-9a-f]{64})" \}', accel)
    require(len(rows) == 22, "GP107 firmware manifest no longer has 22 exact entries")
    require("NV1050_FW_MANIFEST_COUNT 22u" in header, "manifest count constant changed")
    require("mmu-fifo-channel-ce" in accel, "safe Pascal blocker is absent")
    require("return -1;" in accel, "GPU command refusal is absent")

    print("GPU+xHCI evidence: PASS")
    print("artifacts: final ISO/disk hashes; xHCI+EHCI online/read/offline bound")
    print("GPU boundary: 38/38 bring-up, 56/56 passive, linked, physical GPU unverified")


if __name__ == "__main__":
    main()
