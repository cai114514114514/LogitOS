# Structure validation only: synthetic fixtures contain no executable firmware.
# A v2-as-v1 mutant must visibly fail before the positive parser gate can run.
POLARIS_FW_SRC := tests/gpu/amd/polaris/firmware_test.c c/drivers/gpu/amd/polaris/firmware.c
POLARIS_FW_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu

.PHONY: test-polaris-firmware-negctl test-polaris-firmware-host
test-polaris-firmware-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_FW_FLAGS) -DPOLARIS_FW_NEGCTL_ALLOW_V2 $(POLARIS_FW_SRC) -o $(BUILD)/polaris-firmware-negctl
	@if $(BUILD)/polaris-firmware-negctl >$(BUILD)/polaris-firmware-negctl.log 2>&1; then cat $(BUILD)/polaris-firmware-negctl.log; exit 1; fi
	@grep -q 'rc == expected && empty' $(BUILD)/polaris-firmware-negctl.log
	@grep -q '^POLARIS_FIRMWARE: 132 checks, 1 failures$$' $(BUILD)/polaris-firmware-negctl.log
	@echo 'POLARIS_FIRMWARE_NEGCTL: v2 accepted as v1 (exactly one failure)'
	@$(CC) $(POLARIS_FW_FLAGS) -DPOLARIS_FW_NEGCTL_MISSING_EXTENSION $(POLARIS_FW_SRC) -o $(BUILD)/polaris-firmware-neg-extension
	@if $(BUILD)/polaris-firmware-neg-extension >$(BUILD)/polaris-firmware-neg-extension.log 2>&1; then cat $(BUILD)/polaris-firmware-neg-extension.log; exit 1; fi
	@grep -q 'rc == expected && empty' $(BUILD)/polaris-firmware-neg-extension.log
	@grep -q '^POLARIS_FIRMWARE: 132 checks, 1 failures$$' $(BUILD)/polaris-firmware-neg-extension.log
	@echo 'POLARIS_FIRMWARE_NEGCTL: v1.1 missing header extension accepted (exactly one failure)'

test-polaris-firmware-host: test-polaris-firmware-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_FW_FLAGS) -fsanitize=address,undefined $(POLARIS_FW_SRC) -o $(BUILD)/polaris-firmware-host
	@$(BUILD)/polaris-firmware-host

ci-host: test-polaris-firmware-host

# Optional real-file check, separate from hermetic host fixtures. Callers supply
# a provenance-checked directory; no network fetch or installation is implied.
$(BUILD)/check-polaris-firmware: tools/gpu/amd/firmware.c c/drivers/gpu/amd/polaris/firmware.c c/drivers/gpu/amd/polaris/firmware.h
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu \
	    tools/gpu/amd/firmware.c c/drivers/gpu/amd/polaris/firmware.c -o $@

.PHONY: test-polaris-firmware-files
test-polaris-firmware-files: test-polaris-firmware-host $(BUILD)/check-polaris-firmware
	@test -n "$(POLARIS_FIRMWARE_DIR)" || { echo 'POLARIS_FIRMWARE_DIR is required'; exit 2; }
	@$(BUILD)/check-polaris-firmware "$(POLARIS_FIRMWARE_DIR)/polaris10_sdma.bin" \
	    "$(POLARIS_FIRMWARE_DIR)/polaris10_sdma1.bin"
