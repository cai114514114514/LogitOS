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

# This gate boots a deliberately tiny serial-printing real-mode fixture.  It
# proves our catalog is accepted and control reaches its boot image; it cannot
# yet prove the real loader path because stage1.asm deliberately does not exist.
# Each corrupt image must fail both the host parser and the firmware transfer,
# and the negative target is a prerequisite so that evidence cannot be skipped.
MKISO_DIR := $(BUILD)/mkiso
MKISO_FIXTURE := $(MKISO_DIR)/serial-marker.bin
MKISO_IMAGE := $(MKISO_DIR)/logit-mkiso.iso
MKISO_TEST := tests/unit/mkiso_test.py
MKISO_QEMU ?= qemu-system-x86_64

.PHONY: test-mkiso test-mkiso-negctl

$(MKISO_FIXTURE): tests/fixtures/mkiso/serial-marker.asm tests/bootself.mk
	@mkdir -p $(MKISO_DIR)
	nasm -f bin -o $@ $<

$(MKISO_IMAGE): tools/mkiso.py $(MKISO_FIXTURE) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(MKISO_FIXTURE)

# EACH CONTROL HAS TWO HALVES AND THEY ARE DIFFERENT EVIDENCE, which is why the
# fourth field below exists. The PARSER half proves this gate's own instrument
# detects a malformed catalog; all three mutations must fail it, no exceptions.
# The GUEST half proves the FIRMWARE cares about the field -- and measured
# against this host's SeaBIOS on 2026-09-15, it only cares about one of them:
#
#   wrong-platform     parser FAIL   SeaBIOS refuses to boot      -> enforce both
#   bad-checksum       parser FAIL   SeaBIOS BOOTS IT ANYWAY      -> guest half skips
#   emulation-floppy   parser FAIL   SeaBIOS BOOTS IT ANYWAY      -> guest half skips
#
# SeaBIOS validates the platform id, and does not enforce the validation entry's
# 16-bit sum or refuse a media-type byte that lies about an image which is in
# fact a valid no-emulation one. That is a fact about the firmware, not about
# mkiso.py, and requiring the guest half anyway would redden this gate for a
# reason unrelated to the code under test -- which CLAUDE.md names as the noise
# that trains people to ignore red. So those two SKIP LOUDLY, naming what the
# firmware does not check, and the skip line is part of the gate's output rather
# than a silence. If a future firmware does enforce them, flip the field to 1
# and the guest half becomes a requirement again with no other edit.
test-mkiso-negctl: tools/mkiso.py $(MKISO_FIXTURE) $(MKISO_TEST)
	@set -e; failed=0; \
	 for spec in \
	   'bad-checksum:--negctl-bad-catalog-checksum:catalog validation 16-bit sum:0' \
	   'wrong-platform:--negctl-wrong-platform-id:catalog validation platform id:1' \
	   'emulation-floppy:--negctl-emulation-floppy:initial entry media type:0'; do \
	   name=$${spec%%:*}; rest=$${spec#*:}; flag=$${rest%%:*}; rest=$${rest#*:}; \
	   field=$${rest%%:*}; guest=$${rest##*:}; \
	   iso=$(MKISO_DIR)/negctl-$$name.iso; \
	   python3 tools/mkiso.py $$iso --boot-image $(MKISO_FIXTURE) $$flag; \
	   rc=0; python3 $(MKISO_TEST) $$iso >$(MKISO_DIR)/$$name-parser.log 2>&1 || rc=$$?; \
	   cat $(MKISO_DIR)/$$name-parser.log; \
	   if ! { test "$$rc" -eq 1 && grep -Fq "FAIL: $$field" $(MKISO_DIR)/$$name-parser.log; }; then \
	     echo "test-mkiso-negctl: FAIL -- $$name parser control did not fail as required"; failed=1; \
	   fi; \
	   rc=0; python3 $(MKISO_TEST) $$iso --qemu-only --qemu $(MKISO_QEMU) \
	     >$(MKISO_DIR)/$$name-qemu.log 2>&1 || rc=$$?; \
	   cat $(MKISO_DIR)/$$name-qemu.log; \
	   if test "$$guest" -eq 1; then \
	     if ! { test "$$rc" -eq 1 && grep -Fq 'FAIL: QEMU did not observe LOGIT_MKISO_FIXTURE_OK' \
	       $(MKISO_DIR)/$$name-qemu.log; }; then \
	       echo "test-mkiso-negctl: FAIL -- $$name guest control was expected to fail here and did not"; failed=1; \
	     fi; \
	   else \
	     if test "$$rc" -eq 1 && grep -Fq 'FAIL: QEMU did not observe LOGIT_MKISO_FIXTURE_OK' \
	       $(MKISO_DIR)/$$name-qemu.log; then \
	       echo "test-mkiso-negctl: NOTE -- $$name guest control failed here after all;"; \
	       echo "    this firmware now enforces '$$field'. Flip its guest field to 1 in tests/bootself.mk."; \
	     else \
	       echo "SKIP: $$name guest half -- this SeaBIOS boots the mutated image anyway."; \
	       echo "    It does not enforce '$$field'. The parser half above still caught it,"; \
	       echo "    so the mutation is detected; only the firmware's opinion is unavailable here."; \
	     fi; \
	   fi; \
	 done; \
	 test "$$failed" -eq 0 || { echo 'test-mkiso-negctl: FAIL -- a control that must be watched failing was not'; exit 1; }; \
	 echo 'PASS: mkiso controls -- all three caught by the parser, platform-id also refused by SeaBIOS'

test-mkiso: test-mkiso-negctl $(MKISO_IMAGE) $(MKISO_TEST)
	@python3 $(MKISO_TEST) $(MKISO_IMAGE) --qemu $(MKISO_QEMU)
