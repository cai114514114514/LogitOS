# Modern SCSI transport, proved by the same byte checks on two fresh machines.
# The input image is copied by the harness; no shared disk is written by QEMU.
# C_SRC discovers the driver automatically. blk_init supplies early discovery
# because the device-model probe stage occurs after the root filesystem mount.
.PHONY: test-virtio-scsi test-virtio-scsi-negctl test-virtio-scsi-transitional
ci-boot: test-virtio-scsi

test-virtio-scsi: test-virtio-scsi-negctl $(ISO) $(DISK)
	@python3 tests/boot/run-virtio-scsi-test.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/virtio-scsi/modern

test-virtio-scsi-negctl: $(ISO) $(DISK)
	@python3 tests/boot/run-virtio-scsi-test.py --iso $(ISO) --disk $(DISK) --mode negctl --out $(BUILD)/virtio-scsi/negative

# Same modern virtqueues behind transitional PCI ID 0x1004.
test-virtio-scsi-transitional: test-virtio-scsi-negctl $(ISO) $(DISK)
	@python3 tests/boot/run-virtio-scsi-test.py --iso $(ISO) --disk $(DISK) --mode transitional --out $(BUILD)/virtio-scsi/transitional
