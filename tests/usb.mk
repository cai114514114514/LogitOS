# USB test targets.
#
# Kept in their own fragment rather than in the Makefile because several agents
# are editing the Makefile at the same time and a shared-file edit cannot be
# committed without swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/usb.mk
#
# Nothing else is needed to BUILD the driver: C_SRC globs c/drivers, so
# c/drivers/usb/*.c compile and link with no Makefile change at all, and the
# driver registers itself through the device model's linker section rather than
# through a call in kmain. Verified: build/c/drivers/usb/{xhci,xhci_ring,
# usb_desc,usb_core,usb_hid,hid_report}.o appear in the kernel link line.

.PHONY: test-usb test-usb-host test-usb-ring test-usb-desc test-usb-hid

# --- host unit tests: the three pure layers -------------------------------
# Ring index arithmetic, descriptor parsing and HID report parsing are the parts
# of a USB stack that are pure functions of a byte buffer, and they are also the
# parts whose bugs are invisible from inside QEMU (a wrong cycle bit looks like
# dead hardware; a mis-sized field looks like a mis-typed key). So they are
# compiled on the host and driven directly.

test-usb-ring:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/usb_ring_test tests/unit/usb_ring_test.c \
	    c/drivers/usb/xhci_ring.c -Ic/drivers/usb
	@$(BUILD)/usb_ring_test

test-usb-desc:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/usb_desc_test tests/unit/usb_desc_test.c \
	    c/drivers/usb/usb_desc.c -Ic/drivers/usb
	@$(BUILD)/usb_desc_test

test-usb-hid:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/usb_hid_test tests/unit/usb_hid_test.c \
	    c/drivers/usb/hid_report.c -Ic/drivers/usb -Iinclude/abi
	@$(BUILD)/usb_hid_test

test-usb-host: test-usb-ring test-usb-desc test-usb-hid

test-usb: test-usb-host

# The on-device targets (test-usb-os, test-usb-none, test-usb-negctl) land with
# their harnesses in the next commit.
