# Shared physical PCI INTx ownership: real fanout/retirement/RTE production
# bodies with controlled hardware leaves; guest delivery is a separate gate.
.PHONY: test-pci-intx-host test-pci-intx-negctl
ci-host: test-pci-intx-host
test-pci-intx-host: test-pci-intx-negctl
	@python3 tests/unit/pci_intx_run.py --build $(BUILD)/pci-intx-host
test-pci-intx-negctl:
	@python3 tests/unit/pci_intx_run.py --build $(BUILD)/pci-intx-negctl --negative-only

-include tests/pci_intx_guest.mk
