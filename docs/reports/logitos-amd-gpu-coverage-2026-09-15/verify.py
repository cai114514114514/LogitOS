#!/usr/bin/env python3
"""Verify artifact-bound AMD/RV100 and passive GPU coverage evidence."""

from __future__ import annotations

import hashlib
import json
import re
from pathlib import Path


REPORT = Path(__file__).resolve().parent
ROOT = REPORT.parents[1]
EVIDENCE = REPORT / "evidence"
ISO = ROOT / "build-gpu-coverage-final/logit.iso"
ISO_SHA256 = "adbced2ffcb09dbfb1618fd9945d1c89241d5d912507de60ff51704f4ee3870c"


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit("FAIL: " + message)


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
    require(ISO.is_file(), f"missing artifact: {ISO}")
    require(sha256(ISO) == ISO_SHA256, "final ISO hash changed")
    provenance = json.loads((EVIDENCE / "artifact-provenance.json").read_text())
    require(provenance.get("current_loader_source_compiled") is False,
            "shared-loader proof boundary disappeared")
    require(provenance.get("iso_sha256") == ISO_SHA256,
            "artifact provenance is not bound to final ISO")
    expected_objects = {
        "amd_accel.o": "a42b5151fea643f28eefd63aa541255f9e9febcd1d4cebf352e95b036fcf176c",
        "amd_bootfb.o": "e432133ecc8518a7179a19278d7b56ffce7c162b48420b51422378e4f2014301",
        "intel_bootfb.o": "667308cfdec0a5dbf7c7982bf341e093814856e6a4fb09320ce2e4e2bfd097d3",
        "rv100_accel.o": "861d1dc7aa0800d3cd5c1bc439216b413f8b45bb039cdda021ad317b78e9cc93",
    }
    for name, expected in expected_objects.items():
        path = ROOT / "build-gpu-coverage-final/c/drivers/gpu" / name
        require(sha256(path) == expected, f"GPU object changed: {name}")
        require(provenance["gpu_objects"].get(name) == expected,
                f"provenance hash changed: {name}")

    require_markers(EVIDENCE / "test-amd-accel-host.log", (
        "AMD_RV100_NEGCTL: modern GPU reached legacy BARs (exactly one failure)",
        "AMD_RV100_NEGCTL: out-of-scanout fill submitted (exactly one failure)",
        "AMD_RV100: 518 checks, 0 failures",
    ))
    require_markers(EVIDENCE / "test-amd-bootfb-host.log", (
        "AMD_BOOTFB_NEGCTL: start-only ownership accepted partial overlap (exactly one failure)",
        "AMD_BOOTFB_NEGCTL: memory-decode-off device bound (exactly one failure)",
        "AMD_BOOTFB_NEGCTL: mixed memory/I-O BAR was accepted (exactly one failure)",
        "AMD_BOOTFB_NEGCTL: malformed PM capability was dereferenced (exactly one failure)",
        "AMD_BOOTFB: 59 checks, 0 failures",
    ))
    require_markers(EVIDENCE / "test-intel-bootfb-host.log", (
        "INTEL_BOOTFB_NEGCTL: foreign display was claimed (exactly one failure)",
        "INTEL_BOOTFB_NEGCTL: D3 display was accepted (exactly one failure)",
        "INTEL_BOOTFB_NEGCTL: all-ones PCI command was accepted (exactly one failure)",
        "INTEL_BOOTFB_NEGCTL: unowned LFB produced the three expected failures",
        "INTEL_BOOTFB: 31 checks, 0 failures",
    ))

    rv100 = json.loads((EVIDENCE / "rv100-guest/result.json").read_text())
    require(rv100.get("passed") is True, "RV100 guest did not pass")
    require(rv100.get("pci_identity") == "1002:5159", "RV100 PCI identity changed")
    require(rv100.get("driver") == "amd-bootfb", "RV100 did not bind amd-bootfb")
    require(rv100.get("iso_sha256") == ISO_SHA256, "RV100 result is not bound to final ISO")
    require(rv100.get("qemu_device_model_verified") is True,
            "RV100 QEMU model was not verified through QMP")
    require(rv100.get("rv100_fill_copy_canary_verified") is True,
            "RV100 fill/copy canary is not verified")
    require(rv100.get("canary_original_bytes_restored") is True,
            "RV100 canary bytes were not restored")
    require(rv100.get("physical_amd_gpu_verified") is False,
            "RV100 evidence overclaims physical hardware")
    require(rv100.get("modern_amd_gpu_acceleration_verified") is False,
            "RV100 evidence overclaims modern AMD acceleration")
    fields = rv100["accel_fields"]
    expected_fields = {
        "family": "RV100", "pci": "5159", "stage": "active",
        "engine": "rv100-2d-canary-passed", "desktop": "cpu",
        "commands": "2", "fill": "ok", "copy": "ok", "restore": "ok",
    }
    for key, value in expected_fields.items():
        require(fields.get(key) == value, f"RV100 field {key} changed")
    trace_summary = rv100["qemu_trace"]
    require(trace_summary.get("canary_trigger_value") == "0x7000d",
            "RV100 trigger value changed")
    require(trace_summary.get("canary_trigger_writes", 0) >= 2,
            "RV100 trace lacks two canary triggers")
    require(trace_summary.get("fill_master_writes", 0) >= 1,
            "RV100 trace lacks fill master")
    require(trace_summary.get("copy_master_writes", 0) >= 1,
            "RV100 trace lacks copy master")
    require(trace_summary.get("cache_flush_writes", 0) >= 3,
            "RV100 trace lacks three cache flushes")
    scanout = rv100["scanout"]
    require((scanout.get("width"), scanout.get("height")) == (1024, 768),
            "RV100 scanout dimensions changed")
    require(scanout.get("sample_colours", 0) >= 32, "RV100 scanout is flat")

    trace = require_markers(EVIDENCE / "rv100-guest/ati-mmio.trace", (
        "ati_mm_write 4 0x143c  <- 0x7000d",
        "ati_mm_write 4 0x146c  <- 0x52f006de",
        "ati_mm_write 4 0x146c  <- 0x52cc36ff",
        "ati_mm_write 4 0x1714  <- 0xf",
    ))
    require(trace.count("ati_mm_write 4 0x143c  <- 0x7000d") >= 2,
            "raw trace lacks two exact 13x7 trigger writes")
    require(trace.count("ati_mm_write 4 0x1714  <- 0xf") >= 3,
            "raw trace lacks initial/fill/copy cache flushes")

    serial = require_markers(EVIDENCE / "rv100-guest/serial.log", (
        "[amd-accel] family=RV100 pci=5159 stage=active engine=rv100-2d-canary-passed",
        "desktop=cpu commands=2",
        "fill=ok copy=ok restore=ok",
        "[wm] desktop live",
    ))
    require("[amd-accel] family=RV100 pci=5159 stage=blocked" not in serial,
            "RV100 also emitted a blocked marker")

    rage = json.loads((EVIDENCE / "rage128-guest/result.json").read_text())
    require(rage.get("evidence") == "qemu-ati-vga-1002:5046",
            "Rage128 QEMU identity changed")
    require(rage.get("image_sha256") == ISO_SHA256,
            "Rage128 result is not bound to final ISO")
    require((rage.get("width"), rage.get("height")) == (1024, 768),
            "Rage128 scanout dimensions changed")
    require(rage.get("sample_colours", 0) >= 32, "Rage128 scanout is flat")
    require(rage.get("physical_amd_gpu_verified") is False,
            "Rage128 evidence overclaims physical hardware")
    require(rage.get("native_gpu_acceleration_verified") is False,
            "Rage128 evidence overclaims native acceleration")

    kernel_map = require_markers(EVIDENCE / "kernel-map-gpu.txt", (
        "amd_accel.o:(.text)", "amd_accel_prepare",
        "amd_bootfb.o:(.text)", "amd_bootfb_probe",
        "intel_bootfb.o:(.text)", "intel_bootfb_probe",
        "rv100_accel.o:(.text)", "rv100_prepare", "rv100_fill", "rv100_copy",
    ))
    require(kernel_map.count("(logit_drivers)") >= 2,
            "AMD/Intel bootfb driver declarations are not linked")

    product_header = (ROOT / "c/drivers/gpu/amd_accel.h").read_text()
    product = (ROOT / "c/drivers/gpu/amd_accel.c").read_text()
    require("amd_accel_fill" not in product_header and
            "amd_accel_copy" not in product_header,
            "runtime AMD drawing entry points became public without integration gates")
    require("dev->device != RV100_DEVICE_ID" in product,
            "exact RV100 product gate disappeared")
    require("firmware-gpuvm-ring-fence" in product,
            "modern AMD blocker disappeared")
    require("api=rv100-2d-ready" not in product,
            "product log again overclaims a ready drawing API")

    print("AMD GPU coverage evidence: PASS")
    print("RV100: 518/518 + QEMU 1002:5159 fill/copy canary and restore")
    print("Passive: AMD 59/59, Intel 31/31; final ISO hash bound")
    print("Boundary: desktop CPU; physical and modern AMD acceleration unverified")


if __name__ == "__main__":
    main()
