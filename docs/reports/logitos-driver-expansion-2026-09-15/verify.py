#!/usr/bin/env python3
"""Recheck the artifact-bound driver-expansion evidence."""

from __future__ import annotations

import hashlib
import json
from pathlib import Path


REPORT = Path(__file__).resolve().parent
ROOT = REPORT.parents[1]

EXPECTED = {
    ROOT / "build-drivers-storage-usb/logit.iso":
        "2f108cef9c783d0a074dd086600af7c83850a13843bb6073a05aa65934cb5849",
    ROOT / "build-drivers-storage-usb/disk.img":
        "be19e1c03e92b2c6b432a0dac95e8714aba4abbbb2358aa3cd5fdba805f590d8",
}


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def main() -> None:
    for path, expected in EXPECTED.items():
        require(path.is_file(), f"missing artifact: {path}")
        require(sha256(path) == expected, f"artifact hash changed: {path}")

    ehci_path = ROOT / "build-drivers-storage-usb/usb-hotplug/guest/ehci/result.json"
    ehci = json.loads(ehci_path.read_text())
    require(ehci.get("controller") == "ehci", "EHCI result has wrong controller")
    require(ehci.get("post_boot_attach") is True, "EHCI device was not attached after boot")
    require(ehci.get("scsi_capacity_and_mbr_read") is True, "EHCI media read did not complete")
    require(ehci.get("partition_published") == "usb0p1", "EHCI partition was not published")
    require(ehci.get("block_class_offline") is True, "EHCI block device was not offlined")
    require(ehci.get("iso_sha256") == EXPECTED[next(iter(EXPECTED))], "EHCI ISO hash mismatch")
    require(ehci.get("disk_sha256") == list(EXPECTED.values())[1], "EHCI disk hash mismatch")

    xhci_path = ROOT / "build-driver-audit-final/usb-hotplug-xhci-current/xhci/failure.json"
    xhci = json.loads(xhci_path.read_text())
    require(xhci.get("controller") == "xhci", "xHCI failure has wrong controller")
    require(xhci.get("passed") is False, "known xHCI failure unexpectedly marked passing")
    require(xhci.get("iso_sha256") == EXPECTED[next(iter(EXPECTED))], "xHCI ISO hash mismatch")
    require(xhci.get("disk_sha256") == list(EXPECTED.values())[1], "xHCI disk hash mismatch")

    logs = {
        REPORT / "evidence/test-driver-host-final.log": (
            "net_drv_test: 136 checks passed",
            "X79 chipset full: 31 checks, 0 failed",
            "xeon_e5_platform: 45 checks, 0 failed",
            "raptor_lake_platform: 106 checks, 0 failed",
            "NV_BOOTFB: 56 checks, 0 failures",
            "ACPI_INTEGRITY: 15 checks, 0 failures",
            "hpet_test: 18 checks, 0 failed",
        ),
        REPORT / "evidence/test-time-host.log": (
            "TIME-SWITCH-NEGCTL-OK",
            "time_test: 208 checks, 0 failures",
        ),
        REPORT / "evidence/test-hpet-guest.log": (
            "HPET-GUEST-OK: absent -> refused",
            "HPET-GUEST-OK: present -> active, xchecked, PIT restored HPET, 4-core monotonic",
        ),
    }
    for path, markers in logs.items():
        text = path.read_text(errors="replace")
        for marker in markers:
            require(marker in text, f"missing marker {marker!r} in {path}")

    print("driver expansion evidence: PASS")
    print("artifacts: 2 hashes; EHCI online/read/offline bound; xHCI known failure bound")
    print("gates: full host + time publication + HPET absent/present guest")


if __name__ == "__main__":
    main()
