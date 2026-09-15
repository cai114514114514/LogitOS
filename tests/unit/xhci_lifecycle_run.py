#!/usr/bin/env python3
"""Sanitized production xHCI PCI/firmware/DMA lifecycle and fault controls."""
from pathlib import Path
import argparse
import os
import platform
import re
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", type=Path, required=True)
parser.add_argument("--controls-only", action="store_true")
parser.add_argument("--positive-only", action="store_true")
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)

controls = [
    ("IGNORE_BIOS", "BIOS ownership timeout refuses without SMI clear or re-probe"),
    ("EARLY_BME", "BME starts only after handoff reset and base ring programming"),
    ("LATE_BME", "event table is latched only while bus mastering is enabled"),
    ("ERDP_FIRST", "ERSTBA is latched before ERDP is published"),
    ("EVENT_RING_READBACK", "posted-write flush observes event-ring HCE before Run and isolates DMA"),
    ("RUN_HCE", "Run-time HCE refuses start and isolates DMA"),
    ("COMMAND_READBACK", "dropped MEM-only Command write refuses before BAR access"),
    ("RESTORE_FAILURE", "failed Command isolation retains unknown DMA and blocks re-probe"),
    ("REGISTER_FAILURE", "USB registration failure stops and isolates initialized xHCI"),
]
variants = [] if args.controls_only else [("positive", None)]
if not args.positive_only:
    variants += controls

for name, expected in variants:
    source = (repo / "c/drivers/usb/xhci.c").read_text()
    source = source.replace('__asm__ volatile ("mfence" ::: "memory")',
                            '__atomic_thread_fence(__ATOMIC_SEQ_CST)')
    if name == "IGNORE_BIOS":
        old = ('kprintf("[xhci] BIOS ownership did not release; refusing controller\\n");\n'
               '                return -1;')
        new = ('kprintf("[xhci] negative control ignores BIOS ownership\\n");\n'
               '                /* negative control: continue into firmware-owned MMIO */')
        assert source.count(old) == 1
        source = source.replace(old, new)
    elif name == "EARLY_BME":
        old = ('x->pci_command_quiet = (uint16_t)((x->pci_command_old | PCI_CMD_MEM |\n'
               '                                      PCI_CMD_INTX_DIS) & ~PCI_CMD_MASTER);')
        new = ('x->pci_command_quiet = (uint16_t)(x->pci_command_old | PCI_CMD_MEM |\n'
               '                                      PCI_CMD_INTX_DIS | PCI_CMD_MASTER);')
        assert source.count(old) == 1
        source = source.replace(old, new)
    elif name == "LATE_BME":
        enable = ('    dma_wmb();\n'
                  '    if (pci_command_set(x, (uint16_t)(x->pci_command_quiet | PCI_CMD_MASTER)) != 0) {')
        assert source.count(enable) == 1
        source = source.replace(enable,
            '    w64(x->rt, XRT_IR0 + XIR_ERSTBA, dma_address(x->erst));\n' + enable)
        publish = ('    w64(x->rt, XRT_IR0 + XIR_ERSTBA, dma_address(x->erst));\n'
                   '    w64(x->rt, XRT_IR0 + XIR_ERDP, dma_address(evseg));')
        assert source.count(publish) == 1
        source = source.replace(publish,
            '    w64(x->rt, XRT_IR0 + XIR_ERDP, dma_address(evseg));',1)
    elif name == "ERDP_FIRST":
        publish = ('    w64(x->rt, XRT_IR0 + XIR_ERSTBA, dma_address(x->erst));\n'
                   '    w64(x->rt, XRT_IR0 + XIR_ERDP, dma_address(evseg));')
        assert source.count(publish) == 1
        source = source.replace(publish,
            '    w64(x->rt, XRT_IR0 + XIR_ERDP, dma_address(evseg));\n'
            '    w64(x->rt, XRT_IR0 + XIR_ERSTBA, dma_address(x->erst));')
    elif name == "EVENT_RING_READBACK":
        old = 'if (ring_sts & (STS_HSE|STS_HCE)) {'
        assert source.count(old) == 1
        source = source.replace(old,
            'if (0 && (ring_sts & (STS_HSE|STS_HCE))) {')
    elif name == "RUN_HCE":
        old = ('if (sts & (STS_HSE|STS_HCE)) {\n'
               '                kprintf("[xhci] controller error while starting usbsts=%x\\n",sts);')
        assert source.count(old) == 1
        source = source.replace(old,
            'if (0 && (sts & (STS_HSE|STS_HCE))) {\n'
            '                kprintf("[xhci] controller error while starting usbsts=%x\\n",sts);')
    elif name == "COMMAND_READBACK":
        old = "if (pci_command_set(x, x->pci_command_quiet) != 0) {"
        new = "if ((void)pci_command_set(x, x->pci_command_quiet), 0) {"
        assert source.count(old) == 1
        source = source.replace(old, new)
    elif name == "RESTORE_FAILURE":
        old = ("if (!stopped || !isolated) {\n"
               "        xhci_quarantine(x, why);")
        new = ("(void)isolated;\n"
               "    if (!stopped) { /* negative control: ignore failed BME isolation */\n"
               "        xhci_quarantine(x, why);")
        assert source.count(old) == 1
        source = source.replace(old, new)

    core_source = (repo / "c/drivers/usb/usb_core.c").read_text()
    marker = "/* xHCI adapter. EHCI registers its own PCI driver and uses the same core. */"
    assert core_source.count(marker) == 1
    adapter = core_source[core_source.index(marker):]
    if name == "REGISTER_FAILURE":
        old = "if (rc) (void)xhci_shutdown();"
        new = "if (rc) { /* negative control: leave initialized controller running */ }"
        assert adapter.count(old) == 1
        adapter = adapter.replace(old, new)

    source = re.sub(
        r"return \*\(volatile uint32_t\s*\*\)\(([^;]+)\);",
        r"return test_read(\1, 4);",
        source,
    )
    source = re.sub(
        r"\*\(volatile uint32_t\s*\*\)\(([^;]+)\) = v;",
        r"test_write(\1, 4, v);",
        source,
    )
    assert source.count("x->db[slot] = target;") == 1
    source = source.replace("x->db[slot] = target;",
                            "test_write(&x->db[slot], 4, target);")

    variant_dir = build / name
    variant_dir.mkdir(parents=True, exist_ok=True)
    (variant_dir / "xhci_driver.inc").write_text(source)
    (variant_dir / "xhci_probe_driver.inc").write_text(adapter)
    exe = variant_dir / "test"
    include_dirs = [
        "c/drivers/usb", "c/drivers/core", "c/kernel/pci", "c/kernel/mm","c/kernel/mm/phys","c/kernel/mm/virt","c/kernel/mm/cache","c/kernel/mm/reclaim","c/kernel/core","c/kernel/init","c/kernel/diag","c/kernel/sync","c/kernel/init","c/kernel/diag","c/kernel/sync","c/kernel/cpu","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/kernel/cpu/acpi","c/kernel/cpu/irq","c/kernel/cpu/smp","c/kernel/sched","c/drivers/timer","include","tests/unit/dma_driver_stub",
    ]
    command = [
        os.environ.get("CC", "clang"), "-std=c11", "-O1", "-g", "-pthread",
        "-DLOGIT_HOST_TEST", "-ffunction-sections", "-fdata-sections",
        "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
        "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
    ]
    if expected:
        command.append("-DTEST_NEG_" + name)
    command += ["-I" + str(variant_dir)]
    command += ["-I" + str(repo / path) for path in include_dirs]
    command += [str(repo / "tests/unit/xhci_lifecycle_test.c"),
                str(repo / "c/drivers/usb/xhci_ring.c"), "-o", str(exe)]
    if platform.system() == "Darwin":
        command += ["-Wl,-dead_strip"]
    else:
        command += ["-Wl,--gc-sections"]
    subprocess.run(command, cwd=repo, check=True)
    result = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
    (variant_dir / "result.log").write_text(result.stdout + result.stderr)
    failed = [line for line in result.stdout.splitlines() if line.startswith("FAIL: ")]
    if expected:
        wanted = "FAIL: " + expected
        if result.returncode != 1 or failed != [wanted] or result.stderr:
            raise RuntimeError(name + " failed for the wrong reason\n" +
                               result.stdout + result.stderr)
        print("EXPECTED-FAIL xHCI " + name + ": " + expected)
    elif result.returncode:
        raise RuntimeError(result.stdout + result.stderr)
    else:
        print(result.stdout.strip())
