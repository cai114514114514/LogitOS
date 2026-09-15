# Intel 82574 e1000e, deliberately separate from legacy 8254x e1000. The host
# checks cover completion semantics; QEMU exercises real MMIO/NVM/PHY setup
# and two complete 128 KiB transfers, not a register mock or just a PCI ID.
.PHONY: test-e1000e-ring test-e1000e-negctl test-e1000e-guest test-e1000e
test-e1000e-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -DE1000E_NEGCTL_IGNORE_MDIC_ERROR -Ic/drivers/net tests/unit/e1000e_test.c -o $(BUILD)/e1000e-negctl
	@if $(BUILD)/e1000e-negctl >$(BUILD)/e1000e-negctl.log 2>&1; then cat $(BUILD)/e1000e-negctl.log; exit 1; fi
	@grep -q '^FAIL: reject completed MDIO hardware error' $(BUILD)/e1000e-negctl.log
	@grep -q '^E1000E_RING: 13 checks, 1 failures' $(BUILD)/e1000e-negctl.log
	@echo 'E1000E_NEGCTL: rejected MDIC READY+ERROR (exactly one failure)'
test-e1000e-ring: test-e1000e-negctl
	@python3 tests/boot/run-pcnet-test.py --self-test
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Ic/drivers/net tests/unit/e1000e_test.c -o $(BUILD)/e1000e-ring
	@$(BUILD)/e1000e-ring
test-e1000e-guest: test-e1000e-ring $(ISO) $(DISK)
	@python3 tests/boot/run-pcnet-test.py --iso $(ISO) --disk $(DISK) --output $(BUILD)/e1000e-guest --device e1000e --driver e1000e
test-e1000e: test-e1000e-guest
