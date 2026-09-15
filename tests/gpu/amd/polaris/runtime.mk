# Every driver stage is the production translation unit. Only the platform
# hardware boundary is modeled, with separate CPU/GPU memories and an independent
# literal packet executor. This target is never real-firmware/physical evidence.
POLARIS_RUNTIME_SRC := tests/gpu/amd/polaris/runtime_test.c \
 c/drivers/gpu/amd/polaris/runtime.c c/drivers/gpu/amd/polaris/memory/layout.c \
 c/drivers/gpu/amd/polaris/firmware.c c/drivers/gpu/amd/polaris/firmware/bundle.c \
 c/drivers/gpu/amd/polaris/smu/toc.c c/drivers/gpu/amd/polaris/smu/loader.c \
 c/drivers/gpu/amd/polaris/sdma/packet.c c/drivers/gpu/amd/polaris/sdma/queue.c \
 c/drivers/gpu/amd/polaris/sdma/engine.c c/drivers/gpu/amd/polaris/present.c
POLARIS_RUNTIME_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
.PHONY: test-polaris-runtime-negctl test-polaris-runtime-host
test-polaris-runtime-negctl:
	@mkdir -p $(BUILD)/gpu/amd/polaris
	@$(CC) $(POLARIS_RUNTIME_FLAGS) -DPOLARIS_FW_BUNDLE_NEGCTL_JT $(POLARIS_RUNTIME_SRC) -o $(BUILD)/gpu/amd/polaris/runtime-control
	@if $(BUILD)/gpu/amd/polaris/runtime-control >$(BUILD)/gpu/amd/polaris/runtime-control.log 2>&1; then cat $(BUILD)/gpu/amd/polaris/runtime-control.log; exit 1; fi
	@grep -q 'FAIL line .*rc==0' $(BUILD)/gpu/amd/polaris/runtime-control.log
	@grep -q '^POLARIS_RUNTIME: .* checks, 33 failures (host protocol model; no physical GPU proof)$$' $(BUILD)/gpu/amd/polaris/runtime-control.log
	@echo 'POLARIS_RUNTIME_NEGCTL: corrupted staged MEC JT2 prevents composed driver activation'
test-polaris-runtime-host: test-polaris-runtime-negctl
	@mkdir -p $(BUILD)/gpu/amd/polaris
	@$(CC) $(POLARIS_RUNTIME_FLAGS) $(POLARIS_RUNTIME_SRC) -o $(BUILD)/gpu/amd/polaris/runtime
	@$(BUILD)/gpu/amd/polaris/runtime
ci-host: test-polaris-runtime-host
