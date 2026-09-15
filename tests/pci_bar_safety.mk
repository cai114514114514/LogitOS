# BAR sizing writes are destructive unless PCI IO/MEM decode is confirmed off.
.PHONY: test-pci-bar-safety-host
ci-host: test-pci-bar-safety-host
test-pci-bar-safety-host:
	@python3 tests/unit/pci_bar_safety_run.py --build $(BUILD)/pci-bar-safety
