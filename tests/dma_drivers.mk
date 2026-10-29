# DMA driver register/ownership adapters. Include from tests/dma.mk.
# These execute the actual five drivers with disjoint CPU/device addresses;
# use NIC/USB/audio boot gates separately for hardware delivery evidence.
.PHONY: test-dma-drivers test-dma-drivers-negctl test-usb-ring-dma-negctl
.PHONY: test-pci-command-contract test-pci-command-contract-negctl

test-dma-drivers: test-dma-drivers-negctl test-pci-command-contract
	@python3 tests/unit/dma_driver_test.py --build $(BUILD)/dma-drivers --asan

test-dma-drivers-negctl:
	@python3 tests/unit/dma_driver_test.py --build $(BUILD)/dma-drivers-negctl --negative-only

# Real device.c readback model plus production-caller order/guard controls.
test-pci-command-contract: test-pci-command-contract-negctl
	@python3 tests/unit/pci_command_run.py --build $(BUILD)/pci-command --positive-only
test-pci-command-contract-negctl:
	@python3 tests/unit/pci_command_run.py --build $(BUILD)/pci-command-neg --negative-only

# The original ring gate also runs the address regression control.
test-usb-ring: test-usb-ring-dma-negctl
test-usb-ring-dma-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DXRING_DMA_NEGCTL_CPUADDR \
	    -o $(BUILD)/usb_ring_dma_negctl tests/unit/usb_ring_test.c \
	    c/drivers/usb/xhci_ring.c -Ic/drivers/usb
	@set +e; $(BUILD)/usb_ring_dma_negctl >$(BUILD)/usb_ring_dma_negctl.log 2>&1; rc=$$?; \
	    test $$rc -eq 1 && grep -q 'usb_ring_test: 11 FAILURE(S)' $(BUILD)/usb_ring_dma_negctl.log

# Dedicated verification image poisons capture memory before each hardware run
# and observes normal last-close stop for 100 ms. Production builds omit this.
ifeq ($(DMA_CAPTURE_VERIFY),1)
CFLAGS += -DHDA_DMA_CAPTURE_CANARY
endif
$(BUILD)/dma-capture.elf: tests/unit/dma_capture_guest.c c/apps/crt0_cli.asm c/apps/clib.h
	@mkdir -p $(BUILD)/tests/unit
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/tests/unit/dma-capture.crt.o
	$(CC) $(UCFLAGS) -Ic/apps -c $< -o $(BUILD)/tests/unit/dma-capture.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/tests/unit/dma-capture.crt.o $(BUILD)/tests/unit/dma-capture.o
$(BUILD)/dma-capture.aex: $(BUILD)/dma-capture.elf tools/mkaex.py
	python3 tools/mkaex.py --cli $< $@ dma-capture - '*' 150 150 150

DMA_BASE_BUILD ?= build
.PHONY: dma-driver-capture-disk test-dma-drivers-os
dma-driver-capture-disk: $(BUILD)/dma-capture.aex
	$(MAKE) -n -W tools/mkfs.py BUILD=$(DMA_BASE_BUILD) $(DMA_BASE_BUILD)/disk.img > $(BUILD)/capture-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/capture-disk.make $(BUILD)/capture-disk.img $(BUILD)/dma-capture.aex:/bin/dma-capture
test-dma-drivers-os: test-dma-drivers wide-memory-image wide-memory-disk dma-driver-capture-disk
	@test "$(DMA_CAPTURE_VERIFY)" = "1" || { echo "Use DMA_CAPTURE_VERIFY=1 in an independent BUILD"; exit 1; }
	python3 tests/boot/run-dma-drivers.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/dma-driver-results
