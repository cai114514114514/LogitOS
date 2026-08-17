# virtio-rng test targets.
#
# Kept in its own fragment rather than in the Makefile because several agents
# are editing the Makefile at the same time and a shared-file edit cannot be
# committed without swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/virtio_rng.mk
#
# Nothing else is needed to BUILD the driver: C_SRC globs c/drivers, so
# c/drivers/virtio/virtio_rng.c compiles and links with no other Makefile
# change, and the driver registers itself through the device model's linker
# section (DRIVER_DECLARE) rather than through a call in kmain. Verified:
# build/c/drivers/virtio/virtio_rng.o appears in the kernel link line.

.PHONY: test-virtio-rng test-virtio-rng-negctl

# ci-boot, not ci-host: these boot QEMU. The control is a PREREQUISITE of the
# positive, not a sibling in the list: audit_tests.py's NOT_CI drops every
# `test-*-negctl` from what tools/ci.sh runs, on the ground that a control is
# "RUN BY its positive counterpart" -- so naming it beside the positive would
# have excluded it and invoked it from nowhere. The cost is honest and worth
# saying out loud: this device is two QEMU boots in CI, not one.
ci-boot: test-virtio-rng
test-virtio-rng: test-virtio-rng-negctl

# The claim that matters: on a CPU model with NO RDSEED/RDRAND (-cpu qemu64,
# per Makefile:1182-1185's own note that this is where rng.c's hardware path
# is absent), the driver still pulls two independent, non-zero, non-identical
# 16-byte reads off the device -- see run-virtio-rng-test.sh's own header for
# the four properties asserted and why each distinguishes a working device
# from a stub that "succeeds" without doing the work.
test-virtio-rng: $(ISO) $(DISK)
	@bash tests/boot/run-virtio-rng-test.sh $(ISO) $(DISK)

# The negative control: the identical kernel and command line with
# -device virtio-rng-pci removed. The positive assertion (a self-test line on
# serial) must be UNSATISFIABLE here -- run and watched failing (i.e. the
# self-test line correctly absent) before this target was wired in. A control
# that has never been watched failing is not a control.
test-virtio-rng-negctl: $(ISO) $(DISK)
	@ENABLE=0 bash tests/boot/run-virtio-rng-test.sh $(ISO) $(DISK)
