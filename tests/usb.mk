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

.PHONY: test-usb test-usb-host test-usb-ring test-usb-desc test-usb-hid \
        test-usb-os test-usb-none test-usb-both test-usb-negctl

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

# --- on device ------------------------------------------------------------
# The claim that matters is not "the controller initialised". It is that a
# keypress and a mouse movement injected at a USB device reached a ring-3 app.
# run-usb-test.sh boots with -device qemu-xhci -device usb-kbd -device usb-mouse
# AND -machine pc,i8042=off, so there is no PS/2 controller in the machine at
# all: any input the app receives can only have come through this driver.
test-usb-os: $(ISO) $(DISK)
	@bash tests/boot/run-usb-test.sh $(ISO) $(DISK)

# The regression that would take every other line of work down with it: with no
# xHCI device present, the machine must still boot and PS/2 input must still
# work -- every QMP driver in tests/qmp injects PS/2 events. It counts rather
# than detects, so double delivery fails it too.
test-usb-none: $(ISO) $(DISK)
	@bash tests/boot/run-usb-absent-test.sh $(ISO) $(DISK)

# Coexistence: both stacks live at once. The requirement is not "input works"
# but "exactly once" -- two producers on one queue is only safe if neither
# duplicates the other's events, and no "does input work?" test catches that.
test-usb-both: $(ISO) $(DISK)
	@bash tests/boot/run-usb-both-test.sh $(ISO) $(DISK)

# The negative control: the same machine as test-usb-os with the USB devices
# unplugged. Nothing can deliver input, so the positive assertions must not be
# satisfiable. A test that passes with the thing under test taken away is
# measuring something else.
test-usb-negctl: $(ISO) $(DISK)
	@bash tests/boot/run-usb-negctl.sh $(ISO) $(DISK)

test-usb: test-usb-host test-usb-os test-usb-none test-usb-both test-usb-negctl
