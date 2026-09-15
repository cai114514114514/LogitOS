# SMU7 TOC bytes in ordinary RAM; this is not a firmware-loading gate.
POLARIS_SMU_TOC_TEST_SRC := tests/gpu/amd/polaris/smu/toc_test.c c/drivers/gpu/amd/polaris/smu/toc.c
POLARIS_SMU_TOC_TEST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu

.PHONY: test-polaris-smu-toc-negctl test-polaris-smu-toc-host

test-polaris-smu-toc-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_TOC_TEST_FLAGS) -fsanitize=address,undefined \
	    -DPOLARIS_SMU_TOC_NEGCTL_ADDRESS_ORDER $(POLARIS_SMU_TOC_TEST_SRC) \
	    -o $(BUILD)/polaris-smu-toc-negctl
	@if $(BUILD)/polaris-smu-toc-negctl >$(BUILD)/polaris-smu-toc-negctl.log 2>&1; then \
	    cat $(BUILD)/polaris-smu-toc-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*memcmp(output, oracle' $(BUILD)/polaris-smu-toc-negctl.log
	@grep -q '^POLARIS_SMU_TOC: 186 checks, 2 failures$$' $(BUILD)/polaris-smu-toc-negctl.log
	@echo 'POLARIS_SMU_TOC_NEGCTL: swapped high/low GPU address words rejected by literal wire oracle'

test-polaris-smu-toc-host: test-polaris-smu-toc-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_TOC_TEST_FLAGS) -fsanitize=address,undefined \
	    $(POLARIS_SMU_TOC_TEST_SRC) -o $(BUILD)/polaris-smu-toc-host
	@$(BUILD)/polaris-smu-toc-host

ci-host: test-polaris-smu-toc-host
