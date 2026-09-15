# Full production HCD linked by the host fixture; guest gates live alongside
# USB MSC/HID class tests, which require actual QEMU device transactions.
.PHONY: test-ehci-host test-ehci-negctl test-ehci-hid test-ehci-hid-negctl test-ehci-multi
ci-host: test-ehci-host
ci-boot: test-ehci-hid test-ehci-multi
test-ehci-host: test-ehci-negctl
	@python3 tests/unit/ehci_test.py --build $(BUILD)/ehci-host --positive-only
test-ehci-negctl:
	@python3 tests/unit/ehci_test.py --build $(BUILD)/ehci-host --controls-only
test-ehci-hid: test-ehci-hid-negctl test-ehci-host $(ISO) $(DISK)
	@python3 tests/boot/run-ehci-hid-test.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/ehci-hid
test-ehci-hid-negctl: $(ISO) $(DISK)
	@python3 tests/boot/run-ehci-hid-test.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/ehci-hid-negative --negative
test-ehci-multi: test-ehci-hid-negctl test-ehci-host $(ISO) $(DISK)
	@python3 tests/boot/run-ehci-hid-test.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/ehci-multi --dual
