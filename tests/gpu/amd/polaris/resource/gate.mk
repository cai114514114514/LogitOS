POLARIS_RESOURCE_SOURCE := tests/gpu/amd/polaris/resource/test.c \
    c/drivers/gpu/amd/polaris/resource/atom.c \
    c/drivers/gpu/amd/polaris/resource/files.c \
    c/drivers/gpu/amd/polaris/resource/device.c \
    c/drivers/gpu/amd/polaris/device.c \
    c/drivers/gpu/amd/polaris/firmware.c \
    c/drivers/gpu/amd/polaris/firmware/bundle.c \
    c/drivers/gpu/amd/polaris/smu/toc.c

# Only headers actually used by this gate; no user libc or hardware stand-ins
# on the production include path. The kernel services are linked test stubs.
POLARIS_RESOURCE_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror \
    -Ic/drivers/gpu -Ic/drivers/core -Ic/kernel/pci \
    -Ic/kernel/mm/phys -Ic/fs/vfs -Ic/fs/logitfs

.PHONY: test-polaris-resources-host test-polaris-resources-negctl
test-polaris-resources-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_RESOURCE_FLAGS) -fsanitize=address,undefined \
	    -DPOLARIS_RESOURCE_NEGCTL_DROP_RESERVATION $(POLARIS_RESOURCE_SOURCE) \
	    -o $(BUILD)/polaris-resources-negctl
	@if $(BUILD)/polaris-resources-negctl >$(BUILD)/polaris-resources-negctl.log 2>&1; then \
	    cat $(BUILD)/polaris-resources-negctl.log; exit 1; fi
	@grep -q 'FAIL line .*result.firmware.bytes == 65536' $(BUILD)/polaris-resources-negctl.log
	@grep -q 'FAIL line .*report.reservations.firmware.bytes == 65536' $(BUILD)/polaris-resources-negctl.log
	@grep -q '^POLARIS_RESOURCES: .* checks, 4 failures$$' $(BUILD)/polaris-resources-negctl.log
	@echo 'POLARIS_RESOURCES_NEGCTL: omitted firmware reservation caught by literal parser and production discovery oracles'

test-polaris-resources-host: test-polaris-resources-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_RESOURCE_FLAGS) -fsanitize=address,undefined \
	    $(POLARIS_RESOURCE_SOURCE) -o $(BUILD)/polaris-resources-host
	@$(BUILD)/polaris-resources-host
