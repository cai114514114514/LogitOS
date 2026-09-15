# Physical storage compatibility. Host fixtures exercise verbatim production
# discovery/identify/handoff code; QEMU supplies endpoint/topology evidence.
.PHONY: test-storage-hardware-host test-storage-hardware-negctl test-nvme-bridge test-nvme-bridge-uefi test-nvme-4kn test-ahci-sector
ci-host: test-storage-hardware-host
ci-boot: test-nvme-bridge test-nvme-bridge-uefi test-nvme-4kn test-ahci-sector

test-storage-hardware-host: test-storage-hardware-negctl
	@python3 tests/unit/storage_hardware_test.py --build $(BUILD)/storage-hardware/host --positive-only

test-storage-hardware-negctl:
	@python3 tests/unit/storage_hardware_test.py --build $(BUILD)/storage-hardware/host --controls-only

test-nvme-bridge: test-storage-hardware-host $(ISO) $(DISK)
	@python3 tests/boot/run-storage-hardware-test.py --iso $(ISO) --disk $(DISK) --mode nvme-bridge --out $(BUILD)/storage-hardware/nvme-bridge

test-nvme-4kn: test-storage-hardware-host test-dma-block $(ISO) $(DISK)
	@python3 tests/boot/run-storage-hardware-test.py --iso $(ISO) --disk $(DISK) --mode nvme-4kn --out $(BUILD)/storage-hardware/nvme-4kn

test-ahci-sector: test-storage-hardware-host $(ISO) $(DISK)
	@python3 tests/boot/run-storage-hardware-test.py --iso $(ISO) --disk $(DISK) --mode ahci-sector --out $(BUILD)/storage-hardware/ahci-sector

# Same root-port storage path through the self-built UEFI loader. This does not
# turn OVMF evidence into a claim of successful boot on a physical motherboard.
test-nvme-bridge-uefi: test-storage-hardware-host $(ISO) $(ESP_IMG) $(DISK)
	@python3 tests/boot/run-storage-hardware-test.py --iso $(ISO) --esp $(ESP_IMG) --disk $(DISK) --mode nvme-bridge --out $(BUILD)/storage-hardware/nvme-bridge-uefi
