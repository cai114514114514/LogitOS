POLARIS_ENGINE_SRC := tests/gpu/amd/polaris/sdma/engine_test.c c/drivers/gpu/amd/polaris/sdma/engine.c c/drivers/gpu/amd/polaris/sdma/queue.c c/drivers/gpu/amd/polaris/sdma/packet.c
POLARIS_ENGINE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
.PHONY: test-polaris-sdma-engine-negctl test-polaris-sdma-engine-host
test-polaris-sdma-engine-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_ENGINE_FLAGS) -DPOLARIS_ENGINE_NEGCTL_IGNORE_LOAD_STATUS $(POLARIS_ENGINE_SRC) -o $(BUILD)/polaris-sdma-engine-negctl
	@if $(BUILD)/polaris-sdma-engine-negctl >$(BUILD)/polaris-sdma-engine-negctl.log 2>&1; then cat $(BUILD)/polaris-sdma-engine-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*start_engine(&m,&e,&q)==POLARIS_SDMA_ENGINE_PREREQUISITE' $(BUILD)/polaris-sdma-engine-negctl.log
	@grep -q '^POLARIS_SDMA_ENGINE: 177 checks, 1 failures$$' $(BUILD)/polaris-sdma-engine-negctl.log
	@echo 'POLARIS_SDMA_ENGINE_NEGCTL: cached loader status accepted (exactly one failure)'
test-polaris-sdma-engine-host: test-polaris-sdma-engine-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_ENGINE_FLAGS) $(POLARIS_ENGINE_SRC) -o $(BUILD)/polaris-sdma-engine-host
	@$(BUILD)/polaris-sdma-engine-host
ci-host: test-polaris-sdma-engine-host
