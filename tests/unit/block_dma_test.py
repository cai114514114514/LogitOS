#!/usr/bin/env python3
"""Run verbatim NVMe/AHCI command and stop code against explicit DMA/MMIO fixtures.
This tests driver address construction and mapping lifetime, not real hardware
or the DMA API implementation. The boot storage gates provide device evidence.
"""

import argparse
import os
import re
import subprocess
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2])
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--driver-source", type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--controls-only", action="store_true")
mode.add_argument("--positive-only", action="store_true")
args = parser.parse_args()
root = args.root.resolve()
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)
test = Path(__file__).with_name("block_dma_test.c")


def function(source, name):
    match = re.search(
        r"^(?:static )?(?:inline )?[\w *]+\b" + name + r"\([^;]*?\)\n\{", source, re.M
    )
    if not match:
        raise RuntimeError("missing production function " + name)
    start = match.start()
    end = source.index("\n}", match.end()) + 2
    return source[start:end] + "\n"


for driver in ("nvme", "ahci"):
    source_path = (args.driver_source or root / "c/drivers/block") / (driver + ".c")
    source = source_path.read_text()
    structs = (
        ["nvme_sqe", "nvme_cqe", "nvme_q"]
        if driver == "nvme"
        else ["ahci_cmdspec", "ahci_port"]
    )
    types = "\n".join(re.findall(r"^#define [A-Z][^\n]*", source, re.M)) + "\n"
    for name in structs:
        match = re.search(r"^struct " + name + r" \{", source, re.M)
        start = match.start()
        end = source.index("\n}", start) + 2
        end = source.index(";", end) + 1
        types += source[start:end] + "\n"
    if driver == "nvme":
        types = (
            "\n".join(
                re.findall(r"^enum nvme_partial_\w+ \{.*?^\};", source, re.M | re.S)
            )
            + "\n"
            + types
        )
    names = (
        [
            "nvme_begin",
            "nvme_step",
            "nvme_quiesce",
            "nvme_set_transfer",
            "nvme_publish",
            "nvme_prepare_partial_read",
            "nvme_prepare_mapped_transfer",
            "nvme_issue",
            "nvme_release_stopped_request",
            "nvme_complete_mapped_transfer",
            "nvme_complete_partial_transfer",
            "nvme_blk_poll",
            "nvme_free_all",
            "nvme_shutdown",
            "nvme_remove",
        ]
        if driver == "nvme"
        else [
            "ahci_dma_init",
            "wait_clear",
            "port_stop",
            "port_start",
            "port_quiesce",
            "port_recover",
            "ahci_unmap",
            "ahci_issue",
            "ahci_check",
            "ahci_begin",
            "ahci_step",
            "ahci_shutdown",
        ]
    )
    functions = "\n".join(function(source, name) for name in names)
    functions += (
        "\n"
        + re.search(
            r"static const struct driver " + driver + r"_driver = \{.*?\n\};",
            source,
            re.S,
        ).group(0)
        + "\n"
    )
    driver_build = build / driver
    driver_build.mkdir(exist_ok=True)
    (driver_build / "block_dma_types.inc").write_text(types)
    for negative in (
        ("", "old_pointer", "old_stop", "zero_neighbours")
        if driver == "nvme"
        else ("", "old_pointer")
    ):
        if args.controls_only and not negative:
            continue
        if args.positive_only and negative:
            continue
        selected = functions
        if negative == "old_pointer":
            old = (
                "command->prp1 = addr;"
                if driver == "nvme"
                else "uint64_t addr = dma_addr_value(segment->addr);"
            )
            new = (
                "command->prp1 = (uint64_t)(uintptr_t)cpu;"
                if driver == "nvme"
                else "uint64_t addr = (uint64_t)(uintptr_t)s->buf;"
            )
            if selected.count(old) != 1:
                raise RuntimeError("old pointer control site changed")
            selected = selected.replace(old, new)
        if negative == "old_stop":
            old = "if (!g_ready) {"
            if selected.count(old) != 1:
                raise RuntimeError("controller stop control site changed")
            selected = selected.replace(old, "if (0) {")
        if negative == "zero_neighbours":
            selected = "#define NVME_4KN_NEGCTL_ZERO_NEIGHBOURS\n" + selected
        name = driver + ("_" + negative if negative else "")
        (driver_build / "block_dma_functions.inc").write_text(selected)
        executable = driver_build / name
        command = [
            os.environ.get("CC", "cc"),
            "-std=c11",
            "-O1",
            "-g",
            "-Wall",
            "-Wextra",
            "-Wno-unused-function",
            "-fsanitize=address,undefined",
            "-fno-sanitize-recover=all",
            "-DTEST_" + driver.upper(),
            "-I" + str(driver_build),
            "-I" + str(root / "c/drivers/core"),
            "-I" + str(root / "c/drivers/block"),
            str(test),
            "-o",
            str(executable),
        ]
        subprocess.run(command, check=True)
        result = subprocess.run([str(executable)], capture_output=True, text=True)
        (driver_build / (name + ".log")).write_text(result.stdout + result.stderr)
        expected = (
            "NVMe PRP1 is bus address, never CPU pointer"
            if driver == "nvme"
            else "AHCI PRDT follows mapped scatter segments"
        )
        if negative == "old_stop":
            expected = "NVMe controller stop discards stale completion and releases pending mapping"
        if negative == "zero_neighbours":
            expected = "NVMe partial write preserves neighbouring sectors"
        if negative:
            if (
                result.returncode != 1
                or "FAIL: " + expected not in result.stdout
                or "AddressSanitizer" in result.stderr
                or "runtime error:" in result.stderr
            ):
                raise RuntimeError(
                    "invalid negative control "
                    + name
                    + ": "
                    + result.stdout
                    + result.stderr
                )
            print("PASS:", name, "fails the explicit driver assertion")
        else:
            if result.returncode:
                raise RuntimeError(name + ": " + result.stdout + result.stderr)
            print(result.stdout.strip())
    (driver_build / "block_dma_functions.inc").write_text(functions)
