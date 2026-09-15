# Product-wrapper integration over an actual volatile MMIO array. This verifies
# observation/refusal, never hardware acceleration or a working Polaris ring.
POLARIS_INTEGRATION_SRC := tests/gpu/amd/polaris/integration_test.c \
    c/drivers/gpu/amd/accel.c c/drivers/gpu/amd/rv100/engine.c c/drivers/gpu/amd/polaris/device.c
POLARIS_INTEGRATION_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
    -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci -Ic/kernel/core -Ic/kernel/diag -Ic/kernel/gui

.PHONY: test-polaris-integration-negctl test-polaris-integration-host
test-polaris-integration-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_INTEGRATION_FLAGS) -fsanitize=address,undefined \
	    -DPOLARIS_INTEGRATION_NEGCTL_REGISTER_PRESENTER $(POLARIS_INTEGRATION_SRC) \
	    -o $(BUILD)/polaris-integration-negctl
	@if $(BUILD)/polaris-integration-negctl >$(BUILD)/polaris-integration-negctl.log 2>&1; then \
	    cat $(BUILD)/polaris-integration-negctl.log; exit 1; fi
	@grep -q 'presenter == NULL && nonnull_installs == 0' $(BUILD)/polaris-integration-negctl.log
	@grep -q '^POLARIS_INTEGRATION: 259 checks, 3 failures$$' $(BUILD)/polaris-integration-negctl.log
	@echo 'POLARIS_INTEGRATION_NEGCTL: read-only probe cannot register an acceleration presenter'

test-polaris-integration-host: test-polaris-integration-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_INTEGRATION_FLAGS) -fsanitize=address,undefined \
	    $(POLARIS_INTEGRATION_SRC) -o $(BUILD)/polaris-integration-host
	@$(BUILD)/polaris-integration-host

ci-host: test-polaris-integration-host
