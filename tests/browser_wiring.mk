# Source edges plus actual shipping ELF symbols. This does not certify execution.
.PHONY: test-browser-wiring test-browser-wiring-negctl

test-browser-wiring-negctl: $(BUILD)/browser.elf tools/browser_wiring_audit.py tests/unit/browser_wiring_audit_test.py
	@python3 tests/unit/browser_wiring_audit_test.py --elf $(BUILD)/browser.elf --out $(BUILD)/wiring

test-browser-wiring: test-browser-wiring-negctl
	@python3 tools/browser_wiring_audit.py --elf $(BUILD)/browser.elf --json $(BUILD)/wiring/wiring-audit.json
