# Included by tests/pci_intx.mk. The hook only exists in a dedicated test build:
# product kmain and the singleton EDU worked example remain unchanged.
ifeq ($(PCIINTXGUEST),1)
OBJ += $(BUILD)/tests/unit/pci_intx_guest_kernel.o
$(KERNEL): $(BUILD)/tests/unit/pci_intx_guest_kernel.o
$(BUILD)/c/kernel/init/kmain.o: CFLAGS += -Ddev_dump=pci_intx_guest_dev_dump
ifeq ($(PCIINTXGUEST_NEG),first)
$(BUILD)/c/kernel/pci/pci_msi.o: CFLAGS += -DPCI_INTX_NEGCTL_FIRST_ONLY
endif
ifeq ($(PCIINTXGUEST_NEG),mask)
$(BUILD)/c/kernel/pci/pci_msi.o: CFLAGS += -DPCI_INTX_NEGCTL_SKIP_MASK
endif
endif

.PHONY: test-pci-intx-guest test-pci-intx-guest-negctl
ci-boot: test-pci-intx-guest

test-pci-intx-guest: test-pci-intx-guest-negctl $(DISK)
	@$(MAKE) BUILD=$(BUILD)/pci-intx-guest/positive PCIINTXGUEST=1 $(BUILD)/pci-intx-guest/positive/logit.iso
	@python3 tests/boot/run-pci-intx-test.py --iso $(BUILD)/pci-intx-guest/positive/logit.iso --disk $(DISK) --out $(BUILD)/pci-intx-guest/positive/result

test-pci-intx-guest-negctl: $(DISK)
	@$(MAKE) BUILD=$(BUILD)/pci-intx-guest/first PCIINTXGUEST=1 PCIINTXGUEST_NEG=first $(BUILD)/pci-intx-guest/first/logit.iso
	@python3 tests/boot/run-pci-intx-test.py --iso $(BUILD)/pci-intx-guest/first/logit.iso --disk $(DISK) --expect-failure dual-b-payload --out $(BUILD)/pci-intx-guest/first/result
	@$(MAKE) BUILD=$(BUILD)/pci-intx-guest/mask PCIINTXGUEST=1 PCIINTXGUEST_NEG=mask $(BUILD)/pci-intx-guest/mask/logit.iso
	@python3 tests/boot/run-pci-intx-test.py --iso $(BUILD)/pci-intx-guest/mask/logit.iso --disk $(DISK) --expect-failure masked-gsi-cannot-hit-reused-vector --out $(BUILD)/pci-intx-guest/mask/result
