# virtio-balloon test targets.
#
# Own fragment, for the reason tests/virtio_rng.mk gives at its top: the
# Makefile has several writers at once and a shared-file edit cannot be
# committed without swallowing theirs. One line pulls it in:
#
#     -include tests/virtio_balloon.mk
#
# Nothing else is needed to BUILD the driver -- C_SRC globs c/drivers and the
# driver registers itself through the device model's linker section
# (DRIVER_DECLARE), so there is no call in kmain to add.
#
# WHY THIS DEVICE IS WORTH HAVING, in this tree specifically. The reclaim and
# swap subsystem's own note says the pressure it exists to handle "has to be
# MANUFACTURED to test any of it, and the harness was built first" -- and
# today that manufacturing is a small -m at boot (test-swap) or a guest
# program allocating until it fails (mempress.as). Neither can squeeze a
# RUNNING machine. A balloon is what a real hypervisor uses to do exactly
# that, from the host side, without a reboot -- which is the shape the
# TIER1/TIER2 split was written for and has never been driven with.
#
# THE ASSERTION IS CROSS-CHECKED FROM BOTH SIDES, which is the point: the
# guest's own pmm_free_frames() must fall by the number of frames the driver
# says it is holding, AND the host's `query-balloon` must report the same
# machine size minus the same amount. One side alone would be a driver
# agreeing with itself.

.PHONY: test-virtio-balloon test-virtio-balloon-negctl

ci-boot: test-virtio-balloon
test-virtio-balloon: test-virtio-balloon-negctl   # see tests/virtio_rng.mk

test-virtio-balloon: $(ISO) $(DISK)
	@python3 tests/boot/run-virtio-balloon-test.py $(ISO) $(DISK)

# The control: the identical kernel and command line with
# -device virtio-balloon-pci removed. The machine must still boot to
# LOGIT_BOOT_OK -- a driver whose absence breaks the boot is a driver that is
# not optional -- and every positive assertion must become unsatisfiable
# because the self-test line is not there to assert about.
test-virtio-balloon-negctl: $(ISO) $(DISK)
	@python3 tests/boot/run-virtio-balloon-test.py $(ISO) $(DISK) --enable=0
