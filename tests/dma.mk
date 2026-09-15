# DMA controls are prerequisites of positive acceptance, not optional siblings.
.PHONY: test-dma test-dma-negctl test-dma-block test-dma-os
test-dma: test-dma-negctl test-dma-block test-blk-async
test-dma-negctl:
	python3 tests/unit/dma_test.py --build $(BUILD)/dma-core
test-dma-block:
	python3 tests/unit/block_dma_test.py --root . --build $(BUILD)/dma-block
# Run with WIDEVERIFY=1 and an independent BUILD. The memory/PIE matrix uses
# the ordinary DMA implementation and real virtio-blk + NVMe on both firmware.
test-dma-os: test-dma test-wide-memory-os

-include tests/dma_drivers.mk
.PHONY: test-dma-virtio test-dma-virtio-negctl
test-dma: test-dma-virtio test-dma-drivers
test-dma-virtio: test-dma-virtio-negctl
	python3 tests/unit/dma_virtio_run.py --positive
test-dma-virtio-negctl:
	python3 tests/unit/dma_virtio_run.py --negative pci-command-ignore
	python3 tests/unit/dma_virtio_run.py --negative wrong-pci-function
	python3 tests/unit/dma_virtio_run.py --negative queue-cpu-address
	python3 tests/unit/dma_virtio_run.py --negative false-quiescence
	python3 tests/unit/dma_virtio_run.py --negative balloon-cpu-pfn
	python3 tests/unit/dma_virtio_run.py --negative gpu-cpu-backing
	python3 tests/unit/dma_virtio_run.py --negative fb-skip-drain

# Complete reproducible acceptance; both flags are test instrumentation only.
# Example: make -j4 BUILD=/tmp/logitos-dma-check WIDEVERIFY=1 DMA_CAPTURE_VERIFY=1 test-dma-os
.PHONY: test-dma-block-os test-dma-virtio-os test-dma-ime-os
test-dma-os: test-dma-block-os test-dma-virtio-os test-dma-drivers-os test-dma-ime-os
test-dma-block-os: test-dma-block wide-memory-image wide-memory-disk
	python3 tests/boot/run-block-dma.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/dma-block-results
test-dma-virtio-os: test-dma-virtio wide-memory-image wide-memory-disk
	python3 tests/boot/run-dma-virtio.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/dma-virtio-results
test-dma-ime-os: wide-memory-image wide-memory-disk
	python3 tests/boot/run-dma-ime.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/dma-ime-results

# Terminal device-removal probe runs in a separate guest; it deliberately takes
# the private root disk offline and reports only through the resident console.
ifeq ($(WIDEVERIFY),1)
OBJ += $(BUILD)/tests/unit/dma_shutdown_guest_kernel.o
$(KERNEL): $(BUILD)/tests/unit/dma_shutdown_guest_kernel.o
endif
$(BUILD)/dma-shutdown.elf: tests/unit/dma_shutdown_guest.c c/apps/crt0_cli.asm c/apps/clib.h
	@mkdir -p $(BUILD)/tests/unit
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/tests/unit/dma-shutdown.crt.o
	$(CC) $(UCFLAGS) -Ic/apps -Itests/unit -c $< -o $(BUILD)/tests/unit/dma-shutdown.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/tests/unit/dma-shutdown.crt.o $(BUILD)/tests/unit/dma-shutdown.o
$(BUILD)/dma-shutdown.aex: $(BUILD)/dma-shutdown.elf tools/mkaex.py
	python3 tools/mkaex.py --cli $< $@ dma-shutdown - '*' 150 150 150
.PHONY: dma-shutdown-disk test-dma-shutdown-os
dma-shutdown-disk: $(BUILD)/dma-shutdown.aex
	$(MAKE) -n -W tools/mkfs.py BUILD=$(WIDE_BASE_BUILD) $(WIDE_BASE_BUILD)/disk.img > $(BUILD)/shutdown-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/shutdown-disk.make $(BUILD)/shutdown-disk.img $(BUILD)/dma-shutdown.aex:/bin/dma-shutdown
test-dma-shutdown-os: test-dma wide-memory-image dma-shutdown-disk
	python3 tests/boot/run-dma-shutdown.py --build $(BUILD) --disk $(BUILD)/shutdown-disk.img --out $(BUILD)/dma-shutdown-results
test-dma-os: test-dma-shutdown-os

# This recipe executes only after every prerequisite above has succeeded.
test-dma-os:
	python3 tests/boot/dma_report.py --build $(BUILD) --out $(BUILD)/dma-acceptance.json

# The negative source variants run first and must trip the specific lifetime
# and scratch assertions; the positive links the same production VFS code.
.PHONY: test-bkl-vfs-host test-bkl-vfs-negctl test-bkl-io-host
test-bkl-vfs-host: test-bkl-vfs-negctl
	python3 tests/unit/bkl_vfs_run.py --build $(BUILD)/bkl-vfs
test-bkl-vfs-negctl:
	python3 tests/unit/bkl_vfs_run.py --build $(BUILD)/bkl-vfs-neg --negative-only
test-bkl-io-host: test-bkl-vfs-host test-blk-async test-dma-virtio test-dma-drivers

.PHONY: test-bkl-block-host test-bkl-block-negctl
test-bkl-block-host: test-bkl-block-negctl
	python3 tests/unit/bkl_blk_run.py --build $(BUILD)/bkl-block
test-bkl-block-negctl:
	python3 tests/unit/bkl_blk_run.py --build $(BUILD)/bkl-block-neg --negative-only
test-bkl-io-host: test-bkl-block-host

.PHONY: test-bkl-network-host test-bkl-network-negctl
test-bkl-network-negctl:
	python3 tests/unit/bkl_network_run.py --build $(BUILD)/bkl-network-neg --negative-only
test-bkl-network-host: test-bkl-network-negctl
	python3 tests/unit/bkl_network_run.py --build $(BUILD)/bkl-network
test-bkl-io-host: test-bkl-network-host

.PHONY: test-bkl-irq-host test-bkl-irq-negctl
test-bkl-irq-negctl:
	python3 tests/unit/bkl_irq_run.py --build $(BUILD)/bkl-irq-neg --negative-only
test-bkl-irq-host: test-bkl-irq-negctl
	python3 tests/unit/bkl_irq_run.py --build $(BUILD)/bkl-irq
test-bkl-io-host: test-bkl-irq-host

.PHONY: test-bkl-pci-host test-bkl-pci-negctl
test-bkl-pci-negctl:
	python3 tests/unit/bkl_pci_run.py --build $(BUILD)/bkl-pci-neg --negative-only
test-bkl-pci-host: test-bkl-pci-negctl
	python3 tests/unit/bkl_pci_run.py --build $(BUILD)/bkl-pci
test-bkl-io-host: test-bkl-pci-host

.PHONY: test-bkl-route-scratch-host
test-bkl-route-scratch-host:
	python3 tests/unit/bkl_route_scratch_run.py --build $(BUILD)/bkl-route-scratch
test-bkl-io-host: test-bkl-route-scratch-host

# Physical driver coverage and its shared bus/DMA regression gates.
-include tests/driver_expansion.mk
