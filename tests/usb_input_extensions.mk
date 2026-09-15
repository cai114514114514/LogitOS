# Generic physical USB HID support; QEMU proves emulated USB transport only.
USB_INPUT_EXT_SRC := tests/unit/usb_input_extensions_test.c c/drivers/usb/usb_bind.c \
    c/drivers/usb/usb_hid.c c/drivers/usb/hid_report.c c/drivers/usb/hid/generic.c \
    c/drivers/usb/hid/feature.c \
    c/drivers/usb/usb_desc.c
USB_INPUT_EXT_INC := -Ic/drivers/usb -Ic/drivers/core $(KCORE_INC) $(KGUI_INC) $(KMM_INC) \
    -Ic/drivers/timer -Iinclude/abi -Ic/lib/gfx/include
.PHONY: test-usb-input-extensions test-usb-input-extensions-negctl test-usb-tablet-os
ci-host: test-usb-input-extensions
ci-boot: test-usb-tablet-os
test-usb-input-extensions: test-usb-input-extensions-negctl
	@$(CC) -O2 -Wall -Wextra $(USB_INPUT_EXT_INC) $(USB_INPUT_EXT_SRC) -o $(BUILD)/usb_input_extensions_test
	@$(BUILD)/usb_input_extensions_test

# Each old failure is restored in production code and the SAME positive
# assertions must fail. A compiler error or unrelated crash is not evidence.
test-usb-input-extensions-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra $(USB_INPUT_EXT_INC) -DUSB_INPUT_NEGCTL_FIRST_INTERFACE $(USB_INPUT_EXT_SRC) -o $(BUILD)/usb_input_first_negctl
	@! $(BUILD)/usb_input_first_negctl > $(BUILD)/usb-input-first-negctl.log
	@grep -q 'FAIL both composite interfaces bind: got 1, want 2' $(BUILD)/usb-input-first-negctl.log
	@echo 'PASS control: first-interface-only binding failed the two-interface assertion'
	@$(CC) -O2 -Wall -Wextra $(USB_INPUT_EXT_INC) -DUSB_INPUT_NEGCTL_ABSOLUTE_AS_RELATIVE $(USB_INPUT_EXT_SRC) -o $(BUILD)/usb_input_abs_negctl
	@! $(BUILD)/usb_input_abs_negctl > $(BUILD)/usb-input-abs-negctl.log
	@grep -q 'FAIL tablet X range normalized' $(BUILD)/usb-input-abs-negctl.log
	@echo 'PASS control: treating absolute coordinates as deltas failed tablet normalization'

test-usb-tablet-os: test-usb-input-extensions $(ISO) $(DISK)
	@python3 tests/boot/run-usb-tablet-test.py $(ISO) $(DISK)
