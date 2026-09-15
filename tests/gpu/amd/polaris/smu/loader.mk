POLARIS_SMU_LOADER_SRC := tests/gpu/amd/polaris/smu/loader_test.c c/drivers/gpu/amd/polaris/smu/loader.c c/drivers/gpu/amd/polaris/smu/toc.c
POLARIS_SMU_LOADER_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
.PHONY: test-polaris-smu-loader-negctl test-polaris-smu-loader-host
test-polaris-smu-loader-negctl:
	@mkdir -p $(BUILD)/gpu/amd/polaris/smu
	@$(CC) $(POLARIS_SMU_LOADER_FLAGS) -DPOLARIS_SMU_LOADER_NEGCTL_STATUS $(POLARIS_SMU_LOADER_SRC) -o $(BUILD)/gpu/amd/polaris/smu/loader-control
	@if $(BUILD)/gpu/amd/polaris/smu/loader-control >$(BUILD)/gpu/amd/polaris/smu/loader-control.log 2>&1; then cat $(BUILD)/gpu/amd/polaris/smu/loader-control.log; exit 1; fi
	@grep -q 'FAIL line .*POLARIS_SMU_LOAD_TIMEOUT' $(BUILD)/gpu/amd/polaris/smu/loader-control.log
	@grep -q '^POLARIS_SMU_LOADER: .* checks, 5 failures$$' $(BUILD)/gpu/amd/polaris/smu/loader-control.log
	@echo 'POLARIS_SMU_LOADER_NEGCTL: ACK without full SRAM firmware completion rejected'
test-polaris-smu-loader-host: test-polaris-smu-loader-negctl
	@mkdir -p $(BUILD)/gpu/amd/polaris/smu
	@$(CC) $(POLARIS_SMU_LOADER_FLAGS) $(POLARIS_SMU_LOADER_SRC) -o $(BUILD)/gpu/amd/polaris/smu/loader
	@$(BUILD)/gpu/amd/polaris/smu/loader
ci-host: test-polaris-smu-loader-host
