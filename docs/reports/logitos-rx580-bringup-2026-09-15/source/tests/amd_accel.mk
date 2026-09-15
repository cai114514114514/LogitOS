# Exact RV100 2D command path. Polaris read-only bring-up has separate gates;
# modern command submission still needs firmware/GPUVM/ring/fence/reset.
AMD_RV100_SRC := tests/unit/amd_accel_test.c c/drivers/gpu/rv100_accel.c
AMD_RV100_INC := -Ic/drivers/gpu

.PHONY: test-amd-accel-negctl test-amd-accel-host test-amd-accel-guest

test-amd-accel-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DRV100_NEGCTL_BROAD_ID \
	    $(AMD_RV100_INC) $(AMD_RV100_SRC) -o $(BUILD)/amd-rv100-neg-broad
	@if $(BUILD)/amd-rv100-neg-broad >$(BUILD)/amd-rv100-neg-broad.log 2>&1; then \
	    cat $(BUILD)/amd-rv100-neg-broad.log; exit 1; fi
	@grep -q 'rv100_prepare.*== -1' $(BUILD)/amd-rv100-neg-broad.log
	@grep -q '^AMD_RV100: 516 checks, 1 failures$$' $(BUILD)/amd-rv100-neg-broad.log
	@echo 'AMD_RV100_NEGCTL: modern GPU reached legacy BARs (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DRV100_NEGCTL_UNBOUNDED_SURFACE \
	    $(AMD_RV100_INC) $(AMD_RV100_SRC) -o $(BUILD)/amd-rv100-neg-bounds
	@if $(BUILD)/amd-rv100-neg-bounds >$(BUILD)/amd-rv100-neg-bounds.log 2>&1; then \
	    cat $(BUILD)/amd-rv100-neg-bounds.log; exit 1; fi
	@grep -q 'rv100_fill.*1279u.*== -1' $(BUILD)/amd-rv100-neg-bounds.log
	@grep -q '^AMD_RV100: 516 checks, 1 failures$$' $(BUILD)/amd-rv100-neg-bounds.log
	@echo 'AMD_RV100_NEGCTL: out-of-scanout fill submitted (exactly one failure)'
	@$(CC) -std=c11 -O1 -Wall -Wextra -Werror -DRV100_NEGCTL_PRESENT_CORRUPT \
	    $(AMD_RV100_INC) $(AMD_RV100_SRC) -o $(BUILD)/amd-rv100-neg-present
	@if $(BUILD)/amd-rv100-neg-present >$(BUILD)/amd-rv100-neg-present.log 2>&1; then \
	    cat $(BUILD)/amd-rv100-neg-present.log; exit 1; fi
	@grep -q 'rc == 0 && !memcmp' $(BUILD)/amd-rv100-neg-present.log
	@grep -q '^AMD_RV100: 523 checks, 1 failures$$' $(BUILD)/amd-rv100-neg-present.log
	@echo 'AMD_RV100_NEGCTL: corrupted runtime upload rejected by pixel oracle (exactly one failure)'

test-amd-accel-host: test-amd-accel-negctl
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Werror \
	    $(AMD_RV100_INC) $(AMD_RV100_SRC) -o $(BUILD)/amd-rv100-host
	@$(BUILD)/amd-rv100-host

test-amd-accel-guest: test-amd-accel-host $(ISO)
	@python3 tests/boot/run-amd-rv100-accel.py --iso $(ISO) \
	    --out $(BUILD)/amd-rv100-accel-guest

ci-host: test-amd-accel-host
