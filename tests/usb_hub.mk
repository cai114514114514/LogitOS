.PHONY: test-usb-hub-host test-usb-hub-negctl test-usb-hub-guest test-xhci-xfer-host test-xhci-xfer-negctl test-xhci-lifecycle-host test-xhci-lifecycle-negctl
ci-host: test-usb-hub-host
ci-host: test-xhci-xfer-host
ci-host: test-xhci-lifecycle-host
ci-boot: test-usb-hub-guest
test-usb-hub-host: test-usb-hub-negctl
	@python3 tests/unit/usb_hub_run.py --build $(BUILD)/usb-hub-host
test-usb-hub-negctl:
	@python3 tests/unit/usb_hub_run.py --build $(BUILD)/usb-hub-negctl --negative-only
test-usb-hub-guest: test-usb-hub-host $(ISO) $(DISK)
	@USB_HUB_TEST=1 python3 tests/boot/run-usb-tablet-test.py $(ISO) $(DISK)
test-xhci-xfer-host: test-xhci-xfer-negctl
	@python3 tests/unit/xhci_xfer_run.py --build $(BUILD)/xhci-xfer-host
test-xhci-xfer-negctl:
	@python3 tests/unit/xhci_xfer_run.py --build $(BUILD)/xhci-xfer-negctl --negative-only
test-xhci-lifecycle-host: test-xhci-lifecycle-negctl
	@python3 tests/unit/xhci_lifecycle_run.py --build $(BUILD)/xhci-lifecycle-host --positive-only
test-xhci-lifecycle-negctl:
	@python3 tests/unit/xhci_lifecycle_run.py --build $(BUILD)/xhci-lifecycle-negctl --controls-only
