# USB SCSI-transparent Bulk-Only storage. An isolated opt-in kernel hook writes
# only the runner's explicitly marked private USB test disk, never a root disk.
USB_STORAGE_RAM ?= 1G
USB_STORAGE_CPU ?= max
ifeq ($(USBMSCVERIFY),1)
ifneq ($(PCIINTXGUEST),)
$(error USBMSCVERIFY and PCIINTXGUEST must use separate BUILD directories)
endif
OBJ += $(BUILD)/tests/unit/usb_storage_guest.o
$(KERNEL): $(BUILD)/tests/unit/usb_storage_guest.o
$(BUILD)/c/kernel/init/kmain.o: CFLAGS += -Ddev_dump=usb_msc_verify_dev_dump
endif

.PHONY: test-usb-storage-host test-usb-storage-negctl test-usb-storage-xhci test-usb-storage-ehci test-usb-storage-dual
ci-host: test-usb-storage-host
test-usb-storage-host: test-usb-storage-negctl
	@python3 tests/unit/usb_storage_run.py --build $(BUILD)/usb-storage-host --positive-only
test-usb-storage-negctl:
	@python3 tests/unit/usb_storage_run.py --build $(BUILD)/usb-storage-host --controls-only
test-usb-storage-xhci: test-usb-storage-host $(DISK)
	@$(MAKE) BUILD=$(BUILD)/usb-storage-xhci USBMSCVERIFY=1 $(BUILD)/usb-storage-xhci/logit.iso
	@python3 tests/boot/run-usb-storage-test.py --iso $(BUILD)/usb-storage-xhci/logit.iso --disk $(DISK) --out $(BUILD)/usb-storage-xhci/result --controller xhci --ram $(USB_STORAGE_RAM) --cpu $(USB_STORAGE_CPU)
test-usb-storage-ehci: test-usb-storage-host $(DISK)
	@$(MAKE) BUILD=$(BUILD)/usb-storage-ehci USBMSCVERIFY=1 $(BUILD)/usb-storage-ehci/logit.iso
	@python3 tests/boot/run-usb-storage-test.py --iso $(BUILD)/usb-storage-ehci/logit.iso --disk $(DISK) --out $(BUILD)/usb-storage-ehci/result --controller ehci --ram $(USB_STORAGE_RAM) --cpu $(USB_STORAGE_CPU)

test-usb-storage-dual: test-usb-storage-host $(DISK)
	@$(MAKE) BUILD=$(BUILD)/usb-storage-dual USBMSCVERIFY=1 $(BUILD)/usb-storage-dual/logit.iso
	@python3 tests/boot/run-usb-storage-test.py --iso $(BUILD)/usb-storage-dual/logit.iso --disk $(DISK) --out $(BUILD)/usb-storage-dual/result --controller dual --ram $(USB_STORAGE_RAM) --cpu $(USB_STORAGE_CPU)
