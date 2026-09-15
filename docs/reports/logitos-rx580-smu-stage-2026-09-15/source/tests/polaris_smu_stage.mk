# Real parser -> TOC encoder -> RAM staging consumer. No GPU upload is tested.
POLARIS_SMU_STAGE_TEST_SRC := tests/unit/polaris_smu_stage_test.c c/drivers/gpu/polaris_smu_stage.c c/drivers/gpu/polaris_firmware.c c/drivers/gpu/polaris_smu_toc.c
POLARIS_SMU_STAGE_TEST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu

.PHONY: test-polaris-smu-stage-negctl test-polaris-smu-stage-host

test-polaris-smu-stage-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_STAGE_TEST_FLAGS) -fsanitize=address,undefined \
	    -DPOLARIS_SMU_STAGE_NEGCTL_CORRUPT $(POLARIS_SMU_STAGE_TEST_SRC) \
	    -o $(BUILD)/polaris-smu-stage-negctl
	@if $(BUILD)/polaris-smu-stage-negctl >$(BUILD)/polaris-smu-stage-negctl.log 2>&1; then \
	    cat $(BUILD)/polaris-smu-stage-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*memcmp(destination + 12288, fw1 + 256, 60)' $(BUILD)/polaris-smu-stage-negctl.log
	@grep -q '^POLARIS_SMU_STAGE: 105 checks, 2 failures$$' $(BUILD)/polaris-smu-stage-negctl.log
	@echo 'POLARIS_SMU_STAGE_NEGCTL: corrupted second-engine payload rejected by independent copy oracle'

test-polaris-smu-stage-host: test-polaris-smu-stage-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_STAGE_TEST_FLAGS) -fsanitize=address,undefined \
	    $(POLARIS_SMU_STAGE_TEST_SRC) -o $(BUILD)/polaris-smu-stage-host
	@$(BUILD)/polaris-smu-stage-host

ci-host: test-polaris-smu-stage-host

POLARIS_STAGE_TOOL_SRC := tools/stage-polaris-sdma.c c/drivers/gpu/polaris_smu_stage.c c/drivers/gpu/polaris_firmware.c c/drivers/gpu/polaris_smu_toc.c
POLARIS_STAGE_TOOL_HDR := c/drivers/gpu/polaris_smu_stage.h c/drivers/gpu/polaris_firmware.h c/drivers/gpu/polaris_smu_toc.h
$(BUILD)/stage-polaris-sdma: $(POLARIS_STAGE_TOOL_SRC) $(POLARIS_STAGE_TOOL_HDR)
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu $(POLARIS_STAGE_TOOL_SRC) -o $@
$(BUILD)/stage-polaris-sdma-neg: $(POLARIS_STAGE_TOOL_SRC) $(POLARIS_STAGE_TOOL_HDR)
	@mkdir -p $(BUILD)
	@$(CC) -std=c11 -O1 -g -Wall -Wextra -Werror -DPOLARIS_SMU_STAGE_NEGCTL_CORRUPT -Ic/drivers/gpu $(POLARIS_STAGE_TOOL_SRC) -o $@

# Optional pinned real inputs, never fetched by a test. The Python oracle must
# first reject the corrupted build, then check the identical positive output.
.PHONY: test-polaris-smu-stage-files
test-polaris-smu-stage-files: test-polaris-smu-stage-host $(BUILD)/stage-polaris-sdma $(BUILD)/stage-polaris-sdma-neg
	@test -n "$(POLARIS_FIRMWARE_DIR)" || { echo 'POLARIS_FIRMWARE_DIR is required'; exit 2; }
	@python3 tests/unit/polaris_smu_stage_files.py --tool $(BUILD)/stage-polaris-sdma \
	    --negative-tool $(BUILD)/stage-polaris-sdma-neg --firmware-dir "$(POLARIS_FIRMWARE_DIR)" \
	    --out $(BUILD)/polaris-smu-real-files
