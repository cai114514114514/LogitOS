POLARIS_QUEUE_SRC := tests/gpu/amd/polaris/sdma/queue_test.c c/drivers/gpu/amd/polaris/sdma/queue.c c/drivers/gpu/amd/polaris/sdma/packet.c
POLARIS_QUEUE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
.PHONY: test-polaris-sdma-queue-negctl test-polaris-sdma-queue-host
test-polaris-sdma-queue-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_QUEUE_FLAGS) -DPOLARIS_QUEUE_NEGCTL_LATE_COMPLETION $(POLARIS_QUEUE_SRC) -o $(BUILD)/polaris-sdma-queue-negctl
	@if $(BUILD)/polaris-sdma-queue-negctl >$(BUILD)/polaris-sdma-queue-negctl.log 2>&1; then cat $(BUILD)/polaris-sdma-queue-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*polaris_sdma_queue_fill(&q,&dst,0,9,256)==POLARIS_SDMA_QUEUE_QUARANTINED' $(BUILD)/polaris-sdma-queue-negctl.log
	@grep -q '^POLARIS_SDMA_QUEUE: 275 checks, 1 failures$$' $(BUILD)/polaris-sdma-queue-negctl.log
	@echo 'POLARIS_SDMA_QUEUE_NEGCTL: late fence accepted after timeout (exactly one failure)'
test-polaris-sdma-queue-host: test-polaris-sdma-queue-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_QUEUE_FLAGS) $(POLARIS_QUEUE_SRC) -o $(BUILD)/polaris-sdma-queue-host
	@$(BUILD)/polaris-sdma-queue-host
ci-host: test-polaris-sdma-queue-host
