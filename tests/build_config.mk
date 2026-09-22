# SPDX-License-Identifier: MIT
# The control removes the stamp prerequisites from a real C build and must
# retain the old VALUE=1 object after VALUE=2 is requested. A shell/parser error
# cannot satisfy it: the subprocess must fail at that exact stale-object check.
.PHONY: test-build-config test-build-config-negctl
test-build-config: test-build-config-negctl
	@python3 tests/tools/make/config.py

test-build-config-negctl:
	@python3 tests/tools/make/config.py --negative-control

ci-host: test-build-config
