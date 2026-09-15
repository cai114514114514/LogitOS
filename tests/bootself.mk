# The source gate is intentionally host-only: it audits what future loaders must
# produce.  Its two mutations are prerequisites so the positive gate cannot be
# run while silently skipping evidence that the apparatus detects bad inputs.
BOOT_CONTRACT_DIR := $(BUILD)/boot-contract
BOOT_CONTRACT_TEST := $(BOOT_CONTRACT_DIR)/boot_contract_test
BOOT_CONTRACT_SOURCES := c/boot/multiboot2.asm c/boot/boot.asm linker.ld \
    c/kernel/mm/pmm.c c/kernel/gui/fb.c c/kernel/cpu/acpi.c c/kernel/core/kmain.c
BOOT_CONTRACT_FIXTURES := tests/fixtures/bootcontract/multiboot2_bad_checksum.asm \
    tests/fixtures/bootcontract/unexpected_tag_consumer.c

.PHONY: test-boot-contract test-boot-contract-negctl

$(BOOT_CONTRACT_TEST): tests/unit/boot_contract_test.c tests/bootself.mk
	@mkdir -p $(BOOT_CONTRACT_DIR)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -o $@ $<

test-boot-contract-negctl: $(BOOT_CONTRACT_TEST) $(BOOT_CONTRACT_FIXTURES)
	@rc=0; $(BOOT_CONTRACT_TEST) \
	    --multiboot tests/fixtures/bootcontract/multiboot2_bad_checksum.asm \
	    >$(BOOT_CONTRACT_DIR)/bad-checksum.log 2>&1 || rc=$$?; \
	 cat $(BOOT_CONTRACT_DIR)/bad-checksum.log; \
	 test "$$rc" -eq 1 && \
	 grep -Fq 'FAIL: Multiboot2 checksum: magic + architecture + length + checksum = 0x00000001 (expected 0x00000000)' \
	    $(BOOT_CONTRACT_DIR)/bad-checksum.log || \
	 { echo 'test-boot-contract-negctl: FAIL -- checksum mutation was not caught'; exit 1; }; \
	 rc=0; $(BOOT_CONTRACT_TEST) \
	    --extra-consumer tests/fixtures/bootcontract/unexpected_tag_consumer.c \
	    >$(BOOT_CONTRACT_DIR)/unexpected-tag.log 2>&1 || rc=$$?; \
	 cat $(BOOT_CONTRACT_DIR)/unexpected-tag.log; \
	 test "$$rc" -eq 1 && \
	 grep -Fq 'FAIL: unexpected Multiboot2 consumer tag 42 in tests/fixtures/bootcontract/unexpected_tag_consumer.c' \
	    $(BOOT_CONTRACT_DIR)/unexpected-tag.log || \
	 { echo 'test-boot-contract-negctl: FAIL -- extra-consumer mutation was not caught'; exit 1; }; \
	 echo 'PASS: boot-contract negative controls both failed as required'

test-boot-contract: test-boot-contract-negctl $(BOOT_CONTRACT_TEST) $(BOOT_CONTRACT_SOURCES)
	@$(BOOT_CONTRACT_TEST)
