# Pure address planning and ordinary-RAM table encoding, not hardware VM proof.
POLARIS_MEMORY_TEST_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu -fsanitize=address,undefined
POLARIS_LAYOUT_TEST_SRC := tests/gpu/amd/polaris/memory/layout_test.c c/drivers/gpu/amd/polaris/memory/layout.c
POLARIS_GART_TEST_SRC := tests/gpu/amd/polaris/memory/gart_test.c c/drivers/gpu/amd/polaris/memory/gart.c
.PHONY: test-polaris-memory-negctl test-polaris-memory-host
test-polaris-memory-negctl:
	@mkdir -p $(BUILD)/gpu/amd/polaris/memory
	@$(CC) $(POLARIS_MEMORY_TEST_FLAGS) -DPOLARIS_MEMORY_NEGCTL_SCANOUT $(POLARIS_LAYOUT_TEST_SRC) -o $(BUILD)/gpu/amd/polaris/memory/layout-control
	@if $(BUILD)/gpu/amd/polaris/memory/layout-control >$(BUILD)/gpu/amd/polaris/memory/layout-control.log 2>&1; then cat $(BUILD)/gpu/amd/polaris/memory/layout-control.log; exit 1; fi
	@grep -q '^POLARIS_MEMORY_LAYOUT: .* checks, 1 failures$$' $(BUILD)/gpu/amd/polaris/memory/layout-control.log
	@$(CC) $(POLARIS_MEMORY_TEST_FLAGS) -DPOLARIS_GART_NEGCTL_SYSTEM $(POLARIS_GART_TEST_SRC) -o $(BUILD)/gpu/amd/polaris/memory/gart-control
	@if $(BUILD)/gpu/amd/polaris/memory/gart-control >$(BUILD)/gpu/amd/polaris/memory/gart-control.log 2>&1; then cat $(BUILD)/gpu/amd/polaris/memory/gart-control.log; exit 1; fi
	@grep -q '^POLARIS_GART: .* checks, 2 failures$$' $(BUILD)/gpu/amd/polaris/memory/gart-control.log
	@echo 'POLARIS_MEMORY_NEGCTL: scanout overwrite and incorrect SYSTEM PTE bit detected'
test-polaris-memory-host: test-polaris-memory-negctl
	@mkdir -p $(BUILD)/gpu/amd/polaris/memory
	@$(CC) $(POLARIS_MEMORY_TEST_FLAGS) $(POLARIS_LAYOUT_TEST_SRC) -o $(BUILD)/gpu/amd/polaris/memory/layout
	@$(BUILD)/gpu/amd/polaris/memory/layout
	@$(CC) $(POLARIS_MEMORY_TEST_FLAGS) $(POLARIS_GART_TEST_SRC) -o $(BUILD)/gpu/amd/polaris/memory/gart
	@$(BUILD)/gpu/amd/polaris/memory/gart
ci-host: test-polaris-memory-host
