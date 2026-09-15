AMD_NATIVE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ic/drivers/gpu
AMD_NATIVE_SRC := tests/gpu/amd/polaris/native/test.c c/drivers/gpu/amd/polaris/native.c
.PHONY: test-polaris-native-host test-polaris-native-negctl
# The host uses RAM-backed BARs and config callbacks; it proves the actual
# adapter's offsets/bounds/translation refusal, never physical PCI operation.
test-polaris-native-negctl:
	@mkdir -p $(BUILD)/native
	@$(CC) $(AMD_NATIVE_FLAGS) -DPOLARIS_NATIVE_NEGCTL_NO_INVALIDATE $(AMD_NATIVE_SRC) -o $(BUILD)/native/negative
	@if $(BUILD)/native/negative >$(BUILD)/native/negative.log 2>&1; then cat $(BUILD)/native/negative.log; exit 1; fi
	@grep -q '^NATIVE: .* checks, 1 failures$$' $(BUILD)/native/negative.log
	@grep -q 'mmio\[0x2f30/4\]==1' $(BUILD)/native/negative.log
	@echo 'NATIVE_NEGCTL: missing HDP invalidate rejected'

test-polaris-native-host: test-polaris-native-negctl
	@$(CC) $(AMD_NATIVE_FLAGS) $(AMD_NATIVE_SRC) -o $(BUILD)/native/test
	@$(BUILD)/native/test
ci-host: test-polaris-native-host
