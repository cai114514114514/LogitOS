# X79-targeted bring-up uses generic protocols, not a motherboard-name allowlist.
.PHONY: test-blk-late-negctl test-x79-host test-x79-memory
test-blk-async: test-blk-late-negctl
test-blk-late-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(BLKREQ_CFLAGS) -DBLK_PART_NO_ONCE -o $(BUILD)/blk_late_negctl tests/unit/blkreq_test.c c/drivers/block/blkdev.c
	@if $(BUILD)/blk_late_negctl > $(BUILD)/blk-late-negctl.log 2>&1; then cat $(BUILD)/blk-late-negctl.log; exit 1; fi
	@grep -q 'FAIL: late partition probe cannot duplicate registry entries' $(BUILD)/blk-late-negctl.log
	@echo 'BLK_LATE_NEGCTL: duplicate late partition publication rejected'

.PHONY: test-guest-memory-profile test-guest-memory-profile-negctl
ci-host: test-guest-memory-profile
test-guest-memory-profile: test-guest-memory-profile-negctl
	@python3 tests/unit/guest_memory_test.py
test-guest-memory-profile-negctl:
	@mkdir -p $(BUILD)
	@if python3 tests/unit/guest_memory_test.py --negative > $(BUILD)/guest-memory-negctl.log 2>&1; then cat $(BUILD)/guest-memory-negctl.log; exit 1; fi
	@grep -q 'FAIL: 16G must reject low-only payload' $(BUILD)/guest-memory-negctl.log
	@echo 'GUEST_MEMORY_NEGCTL: old 8G-only condition fails 16G evidence assertions'

.PHONY: test-pmm-metadata test-pmm-metadata-negctl
test-pmm-metadata: test-pmm-metadata-negctl
	@python3 tests/unit/pmm_metadata_run.py --build $(BUILD)/pmm-metadata
test-pmm-metadata-negctl:
	@python3 tests/unit/pmm_metadata_run.py --build $(BUILD)/pmm-metadata-neg --negative-only
ci-host: test-pmm-metadata
test-x79-host: test-blk-async test-physmap test-pmm-metadata test-wideva test-guest-memory-profile \
    test-ehci-host test-usb-hub-host test-xhci-xfer-host test-usb-storage-host \
    test-x79-chipset-host test-nvidia-pascal-host test-pci-bar-safety-host test-xeon-e5-host

# WIDEVERIFY keeps the destructive PRIVATE-disk scratch probe out of ordinary
# kernels. Reuse completed application outputs through WIDE_BASE_BUILD.
test-x79-memory: test-x79-host wide-memory-image wide-memory-disk
	@test "$(WIDEVERIFY)" = "1" || { echo 'Use WIDEVERIFY=1 with an independent BUILD'; exit 1; }
	@python3 tests/boot/run-wide-memory.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/x79-memory --ram 16G --cpu SandyBridge
	@python3 tests/boot/run-block-dma.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/x79-storage --ram 16G --cpu SandyBridge

# Separate from WIDEVERIFY: storage tests create their own opt-in kernel and
# explicitly marked private media. HID/hub tests use the ordinary kernel.
.PHONY: test-x79-usb
test-x79-usb: USB_STORAGE_RAM=16G
test-x79-usb: USB_STORAGE_CPU=SandyBridge
test-x79-usb: test-ehci-hid test-ehci-multi test-usb-hub-guest test-usb-storage-xhci test-usb-storage-ehci test-usb-storage-dual
