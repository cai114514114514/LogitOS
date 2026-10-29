# Runtime USB root-port hotplug.  The host gate owns the watched no-CSC and
# event-table publication controls; the guest gate exercises both HCDs after
# the kernel booted.
.PHONY: test-usb-hotplug-host test-usb-hotplug-negctl test-usb-hotplug
ci-host: test-usb-hotplug-host
ci-boot: test-usb-hotplug

test-usb-hotplug-host: test-usb-hotplug-negctl test-xhci-lifecycle-host
	@python3 tests/unit/usb_hotplug_run.py --build $(BUILD)/usb-hotplug/host --positive-only

test-usb-hotplug-negctl:
	@python3 tests/unit/usb_hotplug_run.py --build $(BUILD)/usb-hotplug/host --negative-only

test-usb-hotplug: test-usb-hotplug-host $(ISO) $(DISK)
	@python3 tests/boot/run-usb-hotplug-test.py --iso $(ISO) --disk $(DISK) --out $(BUILD)/usb-hotplug/guest --controller both

