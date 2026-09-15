#!/usr/bin/env python3
"""Run the real PCI Command helper and a readback-removal control."""
from pathlib import Path
import argparse
import os
import re
import subprocess


parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--build", required=True, type=Path)
mode = parser.add_mutually_exclusive_group()
mode.add_argument("--negative-only", action="store_true")
mode.add_argument("--positive-only", action="store_true")
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
out = args.build.resolve()
out.mkdir(parents=True, exist_ok=True)


class ContractError(RuntimeError):
    pass


def ordered(name, text, signature, anchors):
    try:
        pos = text.index(signature)
        for anchor in anchors:
            pos = text.index(anchor, pos + 1)
    except ValueError as exc:
        raise ContractError(f"{name}: missing or reordered {anchor!r}") from exc


def audit_callers(sources):
    contracts = {
        "c/drivers/block/nvme.c": (
            "int nvme_init(void)",
            ["dev_enable_checked(dev, 0)", "dev_bar_map(dev, 0)",
             "uint64_t cap = r64", "REG_CSTS) & 1)",
             "dma_buffer_submit(g_admin.sq_mem)", "dev_enable_checked(dev, 1)",
             "(4u << 20) | 1u"],
        ),
        "c/drivers/core/qemu_edu.c": (
            "static int edu_probe", ["dev_enable_checked(dev, 0)",
             "dev_bar_map(dev, 0)", "EDU_LIVENESS / 4] =", "dev_enable_checked(dev, 1)"],
        ),
        "c/drivers/net/pcnet.c": (
            "int pcnet_probe", ["dev_enable_checked(dev, 0)", "inl(io + R_RESET32)",
             "C_STOP", "dma_alloc_coherent", "dev_enable_checked(dev, 1)",
             "dma_buffer_submit", "csr_write(0, C_INIT)"],
        ),
        "c/drivers/net/e1000e.c": (
            "int e1000e_probe", ["dev_enable_checked(dev, 0)",
             "pci_cfg_write16(dev->bus, dev->slot, dev->func, pm + 4",
             "dev_bar_map(dev, 0)",
             "master_stop()", "CTRL_RESET", "dma_alloc_coherent", "dev_enable_checked(dev, 1)",
             "dma_buffer_submit", "wr(TCTL"],
        ),
        "c/drivers/net/e1000.c": (
            "int e1000_probe", ["dev_enable_checked(dev, 0)", "dev_bar_map(dev, 0)",
             "CTRL_RST", "rx_init()", "dev_enable_checked(dev, 1)",
             "dma_publish_buffers()", "REG_RCTL"],
        ),
        "c/drivers/net/rtl8139.c": (
            "int rtl8139_probe", ["dev_enable_checked(dev, 0)", "R_CR, CR_RST",
             "while (inb", "dev_enable_checked(dev, 1)", "dma_publish_buffers()",
             "R_CR, CR_TE | CR_RE"],
        ),
        "c/drivers/net/rtl8169.c": (
            "int rtl8169_probe", ["dev_enable_checked(dev, 0)", "map_mmio_bar(dev)",
             "R_CR, CR_RST", "while (m8", "dev_enable_checked(dev, 1)",
             "dma_publish_buffers()", "R_CR, CR_TE | CR_RE"],
        ),
        "c/drivers/virtio/virtio.c": (
            "int virtio_init_device", ["dev_enable_checked(dev, 0)", "dev_bar_map(dev, barn)",
             "C_STATUS, 0", "VIRTIO_S_FEATURES_OK", "dev_enable_checked(dev, 1)"],
        ),
    }
    for path, (signature, anchors) in contracts.items():
        text = sources[path]
        for master in (0, 1):
            guard = f"if (dev_enable_checked(dev, {master}) != 0)"
            if guard not in text:
                raise ContractError(f"{path}: checked({master}) result is not a refusal guard")
        ordered(path, text, signature, anchors)

    net = sources["c/drivers/virtio/virtio_net.c"]
    if "virtio_init_device(dev, &vnet" not in net or "virtio_init(dev->device, &vnet" in net:
        raise ContractError("virtio-net: probe must initialize its exact PCI function")

    for path, text in sources.items():
        if re.search(r"\bdev_enable\s*\(", text) or re.search(r"\bdev_disable\s*\(", text):
            raise ContractError(f"{path}: legacy unchecked PCI Command API remains")


caller_paths = [
    "c/drivers/block/nvme.c", "c/drivers/core/qemu_edu.c",
    "c/drivers/net/pcnet.c", "c/drivers/net/e1000e.c", "c/drivers/net/e1000.c",
    "c/drivers/net/rtl8139.c", "c/drivers/net/rtl8169.c",
    "c/drivers/virtio/virtio.c", "c/drivers/virtio/virtio_net.c",
]
callers = {path: (root / path).read_text() for path in caller_paths}
if not args.negative_only:
    audit_callers(callers)
    print("PCI caller ordering: 9 production paths PASS")

if not args.positive_only:
    early_bme = dict(callers)
    early_bme["c/drivers/block/nvme.c"] = early_bme["c/drivers/block/nvme.c"].replace(
        "dev_enable_checked(dev, 0)", "dev_enable_checked(dev, 1)", 1)
    try:
        audit_callers(early_bme)
    except ContractError:
        print("EXPECTED-FAIL nvme-early-bme: MEM-only guard is mandatory")
    else:
        raise RuntimeError("NVMe early-BME control escaped the source contract")

    ignored = dict(callers)
    ignored["c/drivers/net/e1000.c"] = ignored["c/drivers/net/e1000.c"].replace(
        "if (dev_enable_checked(dev, 0) != 0)",
        "if (0 && dev_enable_checked(dev, 0) != 0)", 1)
    try:
        audit_callers(ignored)
    except ContractError:
        print("EXPECTED-FAIL e1000-ignore-error: checked result must gate BAR access")
    else:
        raise RuntimeError("ignored PCI Command result escaped the source contract")

    d0_before_isolation = dict(callers)
    e1000e_guard = (
        '    if (dev_enable_checked(dev, 0) != 0) {\n'
        '        kprintf("[e1000e] PCI Command decode rejected\\n"); return -1;\n'
        '    }\n'
    )
    e1000e_before_bar = "    uint64_t base = dev_bar_map(dev, 0);"
    e1000e_source = d0_before_isolation["c/drivers/net/e1000e.c"]
    if e1000e_source.count(e1000e_guard) != 1 or e1000e_source.count(e1000e_before_bar) != 1:
        raise RuntimeError("e1000e D0/BME ordering control anchor drift")
    e1000e_source = e1000e_source.replace(e1000e_guard, "", 1)
    e1000e_source = e1000e_source.replace(
        e1000e_before_bar, e1000e_guard + e1000e_before_bar, 1)
    d0_before_isolation["c/drivers/net/e1000e.c"] = e1000e_source
    try:
        audit_callers(d0_before_isolation)
    except ContractError:
        print("EXPECTED-FAIL e1000e-d0-before-bme-off: BME isolation precedes PMCSR D0")
    else:
        raise RuntimeError("e1000e D0-before-BME-off control escaped the source contract")

source = (root / "c/drivers/core/device.c").read_text()
anchor = re.compile(
    r"return pci_cfg_read16\(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND\) == wanted\n"
    r"\s+\? 0 : -1;"
)
if len(anchor.findall(source)) != 1:
    raise RuntimeError("PCI Command readback control anchor drift")

variants = [] if args.negative_only else [("positive", source, False)]
if not args.positive_only:
    variants.append((
        "ignore-readback",
        anchor.sub(
            "(void)pci_cfg_read16(dev->bus, dev->slot, dev->func, PCI_CFG_COMMAND);\n"
            "    return 0; /* negative control: trust the write without readback */",
            source,
        ),
        True,
    ))

for name, text, negative in variants:
    build = out / name
    build.mkdir(exist_ok=True)
    device = build / "device.c"
    binary = build / "devmodel_test"
    device.write_text(text)
    command = [
        os.environ.get("CC", "clang"), "-std=gnu11", "-O1", "-g",
        "-Wall", "-Wextra", "-fsanitize=address,undefined",
        "-DLOGIT_HOST_TEST", "-o", str(binary),
        str(root / "tests/unit/devmodel_test.c"), str(device),
        "-I" + str(root / "c/drivers/core"),
        "-I" + str(root / "c/kernel/pci"),
        "-I" + str(root / "tests/unit/pcistub"),
    ]
    subprocess.run(command, check=True, cwd=root)
    result = subprocess.run([str(binary)], capture_output=True, text=True)
    log = result.stdout + result.stderr
    (build / "run.log").write_text(log)
    if not negative:
        if result.returncode or "Device-model tests: 44 checks, 0 failed" not in log:
            raise RuntimeError("positive PCI Command gate failed\n" + log)
        print("PCI Command positive: 44 checks PASS")
        continue

    expected = [
        "FAIL: enable must reject dropped Command bits and restore the old value",
        "FAIL: disable must reject uncleared Command bits without restoring enables",
    ]
    failed = [line for line in result.stdout.splitlines() if line.startswith("FAIL: ")]
    if (result.returncode != 1 or failed != expected or result.stderr or
            "Device-model tests: 44 checks, 2 failed" not in result.stdout):
        raise RuntimeError("readback control failed for the wrong reason\n" + log)
    print("EXPECTED-FAIL ignore-readback: 2 exact Command assertions")
