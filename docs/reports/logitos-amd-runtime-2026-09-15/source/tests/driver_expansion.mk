# Common physical-PC compatibility acceptance. Reached through dma.mk: driver
# bring-up depends on the DMA/device model, and keeping this fragment separate
# lets concurrent app work keep owning the top-level Makefile safely.
-include tests/pcnet.mk
-include tests/e1000e.mk
-include tests/storage_hardware.mk
-include tests/virtio_scsi.mk
-include tests/usb_input_extensions.mk
-include tests/pci_intx.mk
-include tests/xeon_e5.mk
-include tests/raptor_lake.mk
-include tests/x79_platform.mk
-include tests/usb_hub.mk
-include tests/usb_storage.mk
-include tests/usb_hotplug.mk
-include tests/ehci.mk
-include tests/x79_chipset.mk
-include tests/nvidia_pascal.mk
-include tests/amd_bootfb.mk
-include tests/amd_accel.mk
-include tests/amd_present.mk
-include tests/fb_native_present.mk
-include tests/intel_bootfb.mk
-include tests/hda_x79.mk
-include tests/pci_bar_safety.mk
-include tests/x2apic.mk
-include tests/raptor_smp.mk
-include tests/e1000_pch2.mk
-include tests/acpi_integrity.mk
-include tests/hpet.mk

.PHONY: test-pci-hardware test-pci-hardware-negctl test-driver-host test-driver-os
ci-host: test-pci-hardware
test-pci-hardware: test-pci-hardware-negctl
	@python3 tests/unit/pci_hardware_run.py --build $(BUILD)/pci-hardware
test-pci-hardware-negctl:
	@python3 tests/unit/pci_hardware_run.py --build $(BUILD)/pci-hardware-neg --negative-only

# Every negative control is a prerequisite of its positive gate. Guest evidence
# is deliberately separate: host fixtures do not establish hardware operation.
test-driver-host: test-pci-hardware test-pci test-pci-intx-host test-netif test-nic-drv \
    test-pcnet-ring test-e1000e-ring test-storage-hardware-host \
    test-usb-input-extensions test-usb-hid test-dma-virtio test-dma-drivers \
    test-blk-async test-ehci-host test-usb-hub-host test-xhci-xfer-host test-usb-storage-host \
    test-usb-hotplug-host \
    test-x79-chipset-host test-nvidia-pascal-host test-amd-bootfb-host test-amd-accel-host \
    test-intel-bootfb-host test-pci-bar-safety-host test-xeon-e5-host \
    test-raptor-lake-host test-raptor-smp-host \
    test-x2apic-host test-hda-x79-host

test-driver-host: test-e1000-pch2-host
test-driver-host: test-acpi-integrity-host
test-driver-host: test-hpet-host

test-driver-os: test-driver-host test-pcnet-guest test-e1000e-guest \
    test-nvme-bridge test-nvme-bridge-uefi test-ahci-sector \
    test-usb-tablet-os test-virtio-scsi test-virtio-scsi-transitional \
    test-pci-intx-guest test-ehci-hid test-ehci-multi test-usb-hub-guest test-usb-storage-dual

test-driver-os: test-usb-hotplug
test-driver-os: test-hpet-guest
test-driver-os: test-amd-bootfb-guest
test-driver-os: test-amd-accel-guest
