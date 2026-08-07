# Full-system test + commit verification + test-liveness auditing.
#
# In its own fragment for the same reason tests/nic.mk and the others are:
# several lines edit the Makefile at once, and a target added at the bottom of
# a 2400-line file is a merge conflict waiting to happen.
#
#   make test-fullsystem      boot and assert the whole machine works
#   make verify-commit        clone HEAD to a temp dir, build ISO + DISK, run it
#   make check-test-liveness  find tests that cannot fail
#
# test-fullsystem deliberately depends on $(DISK) as well as $(ISO). The ISO is
# the kernel; every ring-3 program lives on the disk image. A target that builds
# only the ISO cannot see a userland that does not compile, and that is exactly
# how a non-compiling c/lib/video file reached HEAD.

.PHONY: test-fullsystem test-freeze verify-commit check-test-liveness

# The .aex the DISK rule packs at the LogitFS root, in the order it packs them.
# Passed to the test so it can read each header's display name on the host and
# require the guest to list -- and launch -- exactly those. Derived from $(APPS)
# rather than restated, so adding an app extends the test automatically instead
# of silently leaving the new one untested.
ROOT_AEX := $(foreach a,$(APPS),$(BUILD)/$(a).aex) $(BROWSER_AEX)

test-fullsystem: $(ISO) $(DISK)
	@bash tests/boot/run-fullsystem-test.sh $(ISO) $(DISK) $(ROOT_AEX)

# The user's freeze repro, as assertions. It had no target at all, which is the
# other half of why it went unnoticed that it asserted nothing: a test nothing
# runs cannot even fail loudly enough to be looked at.
test-freeze: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_freeze.py $(ISO) $(DISK) $(BUILD)/freeze

# The commit gate. Clones HEAD (or REV=...) somewhere else entirely, so the
# working tree's untracked files cannot make a broken commit look fine, and
# builds BOTH artefacts before running the full-system test.
verify-commit:
	@bash tools/verify-commit.sh $(if $(REV),--rev $(REV),) $(VERIFY_ARGS)

# Static audit for assertions that cannot fail. Fails on any test script with no
# failing path at all that is not in tools/test-liveness-allow.txt; everything
# else it finds is printed as a warning.
check-test-liveness:
	@python3 tools/check-test-liveness.py
