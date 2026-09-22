# The controls restore the old dependency fan-out and inline main compilation.
# They must fail the same make-plan oracle before the positive gate may run.
.PHONY: test-build-incremental test-build-incremental-negctl
test-build-incremental-negctl:
	@python3 tests/tools/make/incremental.py --control

test-build-incremental: test-build-incremental-negctl
	@python3 tests/tools/make/incremental.py

ci-host: test-build-incremental
