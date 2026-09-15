# This is a fake register snapshot, not hardware/firmware/queue readiness.
# Exact-offset and no-map rejection oracles do not share core implementation.
POLARIS_PROBE_SRC := tests/gpu/amd/polaris/device_test.c c/drivers/gpu/amd/polaris/device.c
POLARIS_PROBE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci

.PHONY: test-polaris-probe-negctl test-polaris-probe-host
test-polaris-probe-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_PROBE_FLAGS) -DPOLARIS_NEGCTL_VRAM32 $(POLARIS_PROBE_SRC) -o $(BUILD)/polaris-probe-negctl
	@if $(BUILD)/polaris-probe-negctl >$(BUILD)/polaris-probe-negctl.log 2>&1; then cat $(BUILD)/polaris-probe-negctl.log; exit 1; fi
	@grep -q 'f.out.vram_bytes == (8ull << 30)' $(BUILD)/polaris-probe-negctl.log
	@grep -q '^POLARIS_PROBE: 89 checks, 3 failures$$' $(BUILD)/polaris-probe-negctl.log
	@echo 'POLARIS_PROBE_NEGCTL: 32-bit VRAM truncation and false MC coverage caught (three failures)'

test-polaris-probe-host: test-polaris-probe-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_PROBE_FLAGS) -fsanitize=address,undefined $(POLARIS_PROBE_SRC) -o $(BUILD)/polaris-probe-host
	@$(BUILD)/polaris-probe-host

ci-host: test-polaris-probe-host
