# The controls restore the old dependency fan-out and inline main compilation.
# They must fail the same make-plan oracle before the positive gate may run.
.PHONY: test-build-incremental test-build-incremental-negctl
test-build-incremental-negctl:
	@python3 tests/tools/make/incremental.py --control

test-build-incremental: test-build-incremental-negctl
	@python3 tests/tools/make/incremental.py

ci-host: test-build-incremental

# The old permanent phony flag check and normal snapshot-source edge made an
# idle `make -q` report work and a dry-run predict a full disk repack. Restore
# both edges as negative controls before testing the real product rules.
.PHONY: test-build-sysroot-idle test-build-sysroot-idle-negctl
test-build-sysroot-idle-negctl:
	@python3 tests/tools/make/sysroot_idle.py --control

test-build-sysroot-idle: test-build-sysroot-idle-negctl
	@python3 tests/tools/make/sysroot_idle.py

ci-host: test-build-sysroot-idle
