# Transport transcript tests only: no GPU, firmware boot or MMIO adapter.
POLARIS_SMU_MAILBOX_SRC := tests/unit/polaris_smu_mailbox_test.c c/drivers/gpu/polaris_smu_mailbox.c
POLARIS_SMU_MAILBOX_FLAGS := -std=c11 -O1 -g -Wall -Wextra -Werror -Ic/drivers/gpu

.PHONY: test-polaris-smu-mailbox-negctl test-polaris-smu-mailbox-host
test-polaris-smu-mailbox-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_MAILBOX_FLAGS) -DPOLARIS_SMU_NEGCTL_ACCEPT_UNSUPPORTED $(POLARIS_SMU_MAILBOX_SRC) -o $(BUILD)/polaris-smu-mailbox-negctl
	@if $(BUILD)/polaris-smu-mailbox-negctl >$(BUILD)/polaris-smu-mailbox-negctl.log 2>&1; then cat $(BUILD)/polaris-smu-mailbox-negctl.log; exit 1; fi
	@grep -q 'rc == (i == 0 ? POLARIS_SMU_UNSUPPORTED : POLARIS_SMU_REJECTED)' $(BUILD)/polaris-smu-mailbox-negctl.log
	@grep -q '^POLARIS_SMU_MAILBOX: 141 checks, 1 failures$$' $(BUILD)/polaris-smu-mailbox-negctl.log
	@echo 'POLARIS_SMU_MAILBOX_NEGCTL: unsupported response accepted (exactly one failure)'

test-polaris-smu-mailbox-host: test-polaris-smu-mailbox-negctl
	@mkdir -p $(BUILD)
	@$(CC) $(POLARIS_SMU_MAILBOX_FLAGS) -fsanitize=address,undefined $(POLARIS_SMU_MAILBOX_SRC) -o $(BUILD)/polaris-smu-mailbox-host
	@$(BUILD)/polaris-smu-mailbox-host

ci-host: test-polaris-smu-mailbox-host
