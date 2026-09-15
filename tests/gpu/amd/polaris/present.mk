POLARIS_PRESENT_SRC := tests/gpu/amd/polaris/present_test.c c/drivers/gpu/amd/polaris/present.c c/drivers/gpu/amd/polaris/sdma/queue.c c/drivers/gpu/amd/polaris/sdma/packet.c
POLARIS_PRESENT_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
.PHONY: test-polaris-present-negctl test-polaris-present-host
test-polaris-present-negctl:
	@mkdir -p $(BUILD)/gpu/amd/polaris
	@$(CC) $(POLARIS_PRESENT_FLAGS) -DPOLARIS_PRESENT_NEGCTL_SKIP_COPY $(POLARIS_PRESENT_SRC) -o $(BUILD)/gpu/amd/polaris/present-control
	@if $(BUILD)/gpu/amd/polaris/present-control >$(BUILD)/gpu/amd/polaris/present-control.log 2>&1; then cat $(BUILD)/gpu/amd/polaris/present-control.log; exit 1; fi
	@grep -q 'FAIL line .*p.active && q.completed==2' $(BUILD)/gpu/amd/polaris/present-control.log
	@grep -q '^POLARIS_PRESENT: .* checks, 1 failures$$' $(BUILD)/gpu/amd/polaris/present-control.log
	@echo 'POLARIS_PRESENT_NEGCTL: missing GPU copy canary caught'
test-polaris-present-host: test-polaris-present-negctl
	@$(CC) $(POLARIS_PRESENT_FLAGS) $(POLARIS_PRESENT_SRC) -o $(BUILD)/gpu/amd/polaris/present
	@$(BUILD)/gpu/amd/polaris/present
ci-host: test-polaris-present-host
