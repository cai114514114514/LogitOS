---
name: logitos-add-a-driver
description: Add or change a PCI device driver in LogitOS (match table, probe, BARs, interrupts, DMA, diagnostics). Use when touching c/drivers/ or adding support for new hardware.
---

# Add a PCI driver

1. **Read first:** `docs/architecture/device-model.md`, then the driver family page in `docs/drivers/`
   (the docs MCP tool `docs_for_source` finds it from any file under `c/drivers/`).
   `c/drivers/core/qemu_edu.c` is the compact example of every step below.
2. **Match:** a table ending in `DEV_MATCH_END`, built with the helpers in `c/drivers/core/driver.h`
   (`DEV_MATCH_VD`, `DEV_MATCH_CLASS`, `DEV_MATCH_PROGIF`, …). `DRIVER_DECLARE` places the driver in the
   `logit_drivers` section; C files under `c/drivers` are picked up by the `Makefile` source rule.
   Registration order decides who probes first, so a device-specific driver must register before a
   generic class driver.
3. **Probe:** return 0 to claim the device, non-zero to pass it on, and undo your own allocations and
   hardware setup before declining. Keep state with `dev_set_drvdata()`; `dev->match->data` carries the
   table entry's tag.
4. **Resources:** map memory BARs with `dev_bar_map()` or a bounded `dev_bar_map_window()`; enable
   decoding with `dev_enable_checked(dev, 0)` and turn bus mastering on (`1`) only once the device is
   ready for DMA. Prepare PCI configuration access in probe context, never first in an interrupt handler.
5. **Interrupts:** `dev_irq_request()` prefers MSI-X, then MSI, then INTx; handlers acknowledge their own
   controller causes and check their own status on a shared INTx line.
6. **Diagnostics and evidence:** log `[subsystem]` lines a person can read from a serial capture, add a
   host model test and a guest test with a negative control (skill `logitos-write-a-test`), and report
   real-hardware results only when they came from real hardware.

Details: `docs/drivers/overview.md`, `docs/kernel/pci.md`, `docs/kernel/diagnostics.md`.
