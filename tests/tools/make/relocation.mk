# A fresh BUILD misses stale primary-source dependencies. The control restores
# the old object path and must fail before the positive graph check can pass.
.PHONY: test-build-relocation test-build-relocation-negctl
test-build-relocation-negctl:
	@python3 tests/tools/make/relocation.py --control

test-build-relocation: test-build-relocation-negctl
	@python3 tests/tools/make/relocation.py

ci-host: test-build-relocation
