# 82579LM/V is its own PCH2 backend. Host register-contract evidence only:
# QEMU e1000e models 82574, so it must never stand in for an 82579 guest gate.
.PHONY: test-e1000-pch2-host test-e1000-pch2-negctl
test-e1000-pch2-negctl:
	@python3 tests/unit/e1000_pch2_test.py --build $(BUILD)/e1000-pch2-host --controls-only
test-e1000-pch2-host: test-e1000-pch2-negctl
	@python3 tests/unit/e1000_pch2_test.py --build $(BUILD)/e1000-pch2-host --positive-only
