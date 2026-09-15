# Full PF package tests; fixtures/real files are data, never host-executed code.
AMD_BUNDLE_CORE := c/drivers/gpu/amd/polaris/firmware/bundle.c c/drivers/gpu/amd/polaris/firmware.c c/drivers/gpu/amd/polaris/smu/toc.c
AMD_BUNDLE_HEADERS := c/drivers/gpu/amd/polaris/firmware/bundle.h c/drivers/gpu/amd/polaris/firmware.h c/drivers/gpu/amd/polaris/smu/toc.h
AMD_BUNDLE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -Ic/drivers/gpu
AMD_BUNDLE_TEST := tests/gpu/amd/polaris/firmware/bundle_test.c
.PHONY: test-polaris-bundle-host test-polaris-bundle-negctl test-polaris-bundle-files

test-polaris-bundle-negctl:
	@mkdir -p $(BUILD)/firmware
	@$(CC) $(AMD_BUNDLE_FLAGS) -DPOLARIS_FW_BUNDLE_NEGCTL_JT $(AMD_BUNDLE_TEST) $(AMD_BUNDLE_CORE) -o $(BUILD)/firmware/bundle-negative
	@if $(BUILD)/firmware/bundle-negative >$(BUILD)/firmware/negative.log 2>&1; then cat $(BUILD)/firmware/negative.log; exit 1; fi
	@grep -q '^BUNDLE: .* checks, 2 failures$$' $(BUILD)/firmware/negative.log
	@grep -q 'memcmp(dst+off,expected,n)' $(BUILD)/firmware/negative.log
	@echo 'BUNDLE_NEGCTL: independent copy oracle rejects corrupted JT2'

test-polaris-bundle-host: test-polaris-bundle-negctl
	@$(CC) $(AMD_BUNDLE_FLAGS) $(AMD_BUNDLE_TEST) $(AMD_BUNDLE_CORE) -o $(BUILD)/firmware/bundle-test
	@$(BUILD)/firmware/bundle-test

ci-host: test-polaris-bundle-host

$(BUILD)/firmware/bundle: tools/gpu/amd/bundle.c $(AMD_BUNDLE_CORE) $(AMD_BUNDLE_HEADERS)
	@mkdir -p $(dir $@)
	@$(CC) $(AMD_BUNDLE_FLAGS) tools/gpu/amd/bundle.c $(AMD_BUNDLE_CORE) -o $@
$(BUILD)/firmware/bundle-negative-tool: tools/gpu/amd/bundle.c $(AMD_BUNDLE_CORE) $(AMD_BUNDLE_HEADERS)
	@mkdir -p $(dir $@)
	@$(CC) $(AMD_BUNDLE_FLAGS) -DPOLARIS_FW_BUNDLE_NEGCTL_JT tools/gpu/amd/bundle.c $(AMD_BUNDLE_CORE) -o $@

test-polaris-bundle-files: test-polaris-bundle-host $(BUILD)/firmware/bundle $(BUILD)/firmware/bundle-negative-tool
	@test -n "$(POLARIS_FIRMWARE_DIR)" || { echo 'POLARIS_FIRMWARE_DIR is required'; exit 2; }
	@python3 tests/gpu/amd/polaris/firmware/files.py --tool $(BUILD)/firmware/bundle --negative-tool $(BUILD)/firmware/bundle-negative-tool --firmware-dir "$(POLARIS_FIRMWARE_DIR)" --out $(BUILD)/firmware/evidence
