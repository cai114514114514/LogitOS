# Pure SDMA v3 packet construction. This gate cannot prove hardware submission.
POLARIS_SDMA_TEST_SRC := tests/gpu/amd/polaris/sdma/packet_test.c c/drivers/gpu/amd/polaris/sdma/packet.c
POLARIS_SDMA_TEST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu

.PHONY: test-polaris-sdma-negctl test-polaris-sdma-host

test-polaris-sdma-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SDMA_TEST_FLAGS) -fsanitize=address,undefined \
	    -DPOLARIS_SDMA_NEGCTL_COUNT_MINUS_ONE $(POLARIS_SDMA_TEST_SRC) \
	    -o $(BUILD)/polaris-sdma-negctl
	@if $(BUILD)/polaris-sdma-negctl >$(BUILD)/polaris-sdma-negctl.log 2>&1; then \
	    cat $(BUILD)/polaris-sdma-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*memcmp(words, copy_oracle' $(BUILD)/polaris-sdma-negctl.log
	@grep -q '^POLARIS_SDMA: 208 checks, 3 failures$$' $(BUILD)/polaris-sdma-negctl.log
	@echo 'POLARIS_SDMA_NEGCTL: SDMA v4 count-minus-one rejected by literal v3 oracle'

test-polaris-sdma-host: test-polaris-sdma-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SDMA_TEST_FLAGS) -fsanitize=address,undefined \
	    $(POLARIS_SDMA_TEST_SRC) -o $(BUILD)/polaris-sdma-host
	@$(BUILD)/polaris-sdma-host

ci-host: test-polaris-sdma-host
