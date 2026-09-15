# Invocable alone or included by driver expansion. The actual class driver and
# consumer spies are linked; this is not a parser-only support claim.
USB_HID_GENERIC_SRC := tests/drivers/usb/hid/generic_test.c c/drivers/usb/usb_bind.c \
 c/drivers/usb/usb_hid.c c/drivers/usb/hid_report.c c/drivers/usb/hid/generic.c \
 c/drivers/usb/hid/feature.c c/drivers/usb/usb_desc.c
USB_HID_GENERIC_INC := -Ic/drivers/usb -Ic/drivers/core -Ic/kernel -Ic/kernel/core \
 -Ic/kernel/gui -Ic/kernel/gui/fb -Ic/kernel/mm/phys -Ic/kernel/diag -Ic/kernel/mm -Ic/drivers/timer -Iinclude/abi -Ic/lib/gfx/include
USB_HID_GENERIC_FLAGS := -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined
.PHONY: test-usb-hid-generic test-usb-hid-generic-negative test-usb-hid-mode-negative
ci-host: test-usb-hid-generic
test-usb-hid-generic: test-usb-hid-generic-negative test-usb-hid-mode-negative
	@$(CC) $(USB_HID_GENERIC_FLAGS) $(USB_HID_GENERIC_INC) $(USB_HID_GENERIC_SRC) -o $(BUILD)/hid-generic
	@$(BUILD)/hid-generic
test-usb-hid-generic-negative:
	@mkdir -p $(BUILD)
	@$(CC) $(USB_HID_GENERIC_FLAGS) $(USB_HID_GENERIC_INC) -DUSB_HID_NEGCTL_STALE_CONTACT $(USB_HID_GENERIC_SRC) -o $(BUILD)/hid-generic-negative
	@! $(BUILD)/hid-generic-negative > $(BUILD)/hid-generic-negative.log
	@grep -q 'FAIL replacement contact ID reanchors without teleport' $(BUILD)/hid-generic-negative.log
	@echo 'PASS control: inherited stale touch anchor fails actual WM position assertion'

test-usb-hid-mode-negative:
	@mkdir -p $(BUILD)
	@$(CC) $(USB_HID_GENERIC_FLAGS) $(USB_HID_GENERIC_INC) -DUSB_HID_NEGCTL_SKIP_MODE $(USB_HID_GENERIC_SRC) -o $(BUILD)/hid-mode-negative
	@! $(BUILD)/hid-mode-negative > $(BUILD)/hid-mode-negative.log
	@grep -q 'FAIL touchpad mode negotiated before input' $(BUILD)/hid-mode-negative.log
	@echo 'PASS control: omitted mode request prevents touchpad activation'
