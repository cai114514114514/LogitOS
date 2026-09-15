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
# yet prove the real loader path because preload.asm deliberately does not exist.
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

# El Torito gives preload one native 2,048-byte CD sector even though the
# catalog spells that preload as four 512-byte units.  The split below is not
# inherited from the MBR's nonexistent limit: it freezes the firmware-loaded
# surface at one CD sector, then makes all growth cross an explicit, measured
# AH=42h handoff.  The 1,152-sector probe asks SeaBIOS for 576 KiB and checks a
# controlled sentinel at the far end, so its result measures delivered bytes
# rather than trusting the catalog field.
BIOS_PRELOAD_DIR := $(BUILD)/bios-preload
BIOS_PRELOAD_BIN := $(BIOS_PRELOAD_DIR)/preload.bin
BIOS_PRELOAD_BAD_DAP_BIN := $(BIOS_PRELOAD_DIR)/preload-bad-dap.bin
BIOS_PRELOAD_WRONG_DRIVE_BIN := $(BIOS_PRELOAD_DIR)/preload-wrong-drive.bin
BIOS_LOADER_BIN := $(BIOS_PRELOAD_DIR)/loader-marker.bin
BIOS_PRELOAD_IMAGE := $(BIOS_PRELOAD_DIR)/preload.iso
BIOS_PRELOAD_BAD_LBA_IMAGE := $(BIOS_PRELOAD_DIR)/preload-bad-lba.iso
BIOS_PRELOAD_BAD_DAP_IMAGE := $(BIOS_PRELOAD_DIR)/preload-bad-dap.iso
BIOS_PRELOAD_WRONG_DRIVE_IMAGE := $(BIOS_PRELOAD_DIR)/preload-wrong-drive.iso
BIOS_CATALOG_PROBE_SECTORS := 1152
BIOS_CATALOG_PROBE_BIN := $(BIOS_PRELOAD_DIR)/catalog-load-$(BIOS_CATALOG_PROBE_SECTORS).bin
BIOS_CATALOG_PROBE_IMAGE := $(BIOS_PRELOAD_DIR)/catalog-load-$(BIOS_CATALOG_PROBE_SECTORS).iso
BIOS_PRELOAD_TEST := tests/unit/bios_preload_test.py
BIOS_PRELOAD_QEMU ?= qemu-system-x86_64

.PHONY: test-bios-preload test-bios-preload-negctl test-bios-preload-probe

$(BIOS_PRELOAD_BIN): c/boot/bios/preload.asm tests/bootself.mk
	@mkdir -p $(BIOS_PRELOAD_DIR)
	nasm -f bin -o $@ $<

$(BIOS_PRELOAD_BAD_DAP_BIN): c/boot/bios/preload.asm tests/bootself.mk
	@mkdir -p $(BIOS_PRELOAD_DIR)
	nasm -f bin -DPRELOAD_NEGCTL_BAD_DAP -o $@ $<

$(BIOS_PRELOAD_WRONG_DRIVE_BIN): c/boot/bios/preload.asm tests/bootself.mk
	@mkdir -p $(BIOS_PRELOAD_DIR)
	nasm -f bin -DPRELOAD_NEGCTL_WRONG_DRIVE -o $@ $<

$(BIOS_LOADER_BIN): tests/fixtures/bios/loader-marker.asm tests/bootself.mk
	@mkdir -p $(BIOS_PRELOAD_DIR)
	nasm -f bin -o $@ $<

$(BIOS_PRELOAD_IMAGE): tools/mkiso.py $(BIOS_PRELOAD_BIN) $(BIOS_LOADER_BIN) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_PRELOAD_BIN) --loader $(BIOS_LOADER_BIN)

$(BIOS_PRELOAD_BAD_LBA_IMAGE): tools/mkiso.py $(BIOS_PRELOAD_BIN) $(BIOS_LOADER_BIN) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_PRELOAD_BIN) --loader $(BIOS_LOADER_BIN) \
	    --negctl-loader-lba-plus-one

$(BIOS_PRELOAD_BAD_DAP_IMAGE): tools/mkiso.py $(BIOS_PRELOAD_BAD_DAP_BIN) $(BIOS_LOADER_BIN) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_PRELOAD_BAD_DAP_BIN) --loader $(BIOS_LOADER_BIN)

$(BIOS_PRELOAD_WRONG_DRIVE_IMAGE): tools/mkiso.py $(BIOS_PRELOAD_WRONG_DRIVE_BIN) $(BIOS_LOADER_BIN) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_PRELOAD_WRONG_DRIVE_BIN) --loader $(BIOS_LOADER_BIN)

$(BIOS_CATALOG_PROBE_BIN): tests/fixtures/bios/catalog-load-probe.asm tests/bootself.mk
	@mkdir -p $(BIOS_PRELOAD_DIR)
	nasm -f bin -DPROBE_SECTORS=$(BIOS_CATALOG_PROBE_SECTORS) -o $@ $<

$(BIOS_CATALOG_PROBE_IMAGE): tools/mkiso.py $(BIOS_CATALOG_PROBE_BIN) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_CATALOG_PROBE_BIN)

# Run the ordinary positive oracle against every mutation and require its
# exact failure.  The wrong-LBA and malformed-DAP controls are the requested
# transfer failures; wrong-drive is the extra control because losing DL while
# setting up segments is a silent and common real-mode error.  Each must show
# preload spoke and loader did not, so a QEMU startup failure cannot satisfy it.
test-bios-preload-negctl: $(BIOS_PRELOAD_BAD_LBA_IMAGE) $(BIOS_PRELOAD_BAD_DAP_IMAGE) \
    $(BIOS_PRELOAD_WRONG_DRIVE_IMAGE) $(BIOS_PRELOAD_TEST)
	@set -e; \
	 if ! command -v $(BIOS_PRELOAD_QEMU) >/dev/null 2>&1; then \
	   echo 'SKIP: test-bios-preload-negctl requires $(BIOS_PRELOAD_QEMU); install QEMU to watch the guest controls fail'; \
	   exit 0; \
	 fi; \
	 failed=0; \
	 for spec in \
	   'bad-lba:$(BIOS_PRELOAD_BAD_LBA_IMAGE)' \
	   'bad-dap:$(BIOS_PRELOAD_BAD_DAP_IMAGE)' \
	   'wrong-drive:$(BIOS_PRELOAD_WRONG_DRIVE_IMAGE)'; do \
	   name=$${spec%%:*}; iso=$${spec#*:}; log=$(BIOS_PRELOAD_DIR)/$$name.log; \
	   rc=0; python3 $(BIOS_PRELOAD_TEST) $$iso --loader $(BIOS_LOADER_BIN) \
	     --qemu-only --qemu $(BIOS_PRELOAD_QEMU) >$$log 2>&1 || rc=$$?; \
	   cat $$log; \
	   if ! { test "$$rc" -eq 1 && \
	     grep -Fq 'FAIL: QEMU observed LOGIT_BIOS_PRELOAD_OK but not LOGIT_BIOS_LOADER_OK' $$log; }; then \
	     echo "test-bios-preload-negctl: FAIL -- $$name did not fail at the handoff as required"; \
	     failed=1; \
	   else \
	     echo "PASS: $$name control was watched failing before loader"; \
	   fi; \
	 done; \
	 test "$$failed" -eq 0 || exit 1; \
	 echo 'PASS: bios-preload negative controls all failed as required'

test-bios-preload-probe: $(BIOS_CATALOG_PROBE_IMAGE) $(BIOS_PRELOAD_TEST)
	@python3 $(BIOS_PRELOAD_TEST) $(BIOS_CATALOG_PROBE_IMAGE) \
	    --catalog-probe-sectors $(BIOS_CATALOG_PROBE_SECTORS) --qemu $(BIOS_PRELOAD_QEMU)

test-bios-preload: test-bios-preload-negctl test-bios-preload-probe \
    $(BIOS_PRELOAD_IMAGE) $(BIOS_PRELOAD_TEST)
	@python3 $(BIOS_PRELOAD_TEST) $(BIOS_PRELOAD_IMAGE) --loader $(BIOS_LOADER_BIN) \
	    --qemu $(BIOS_PRELOAD_QEMU)

# The loader stops at a fixture handoff in real mode.  This is deliberate: the
# block-format instrument must be independently useful before ELF loading and
# protected-mode entry exist, otherwise a kernel failure cannot distinguish a
# bad machine description from a bad handoff.
BIOS_MB2_DIR := $(BUILD)/bios-mb2
BIOS_MB2_PRELOAD := $(BIOS_MB2_DIR)/preload.bin
BIOS_MB2_LOADER := $(BIOS_MB2_DIR)/loader.bin
BIOS_MB2_A20_LOADER := $(BIOS_MB2_DIR)/loader-a20-skip.bin
BIOS_MB2_TRUNC_LOADER := $(BIOS_MB2_DIR)/loader-e820-truncated.bin
BIOS_MB2_BAD_RSDP_LOADER := $(BIOS_MB2_DIR)/loader-rsdp-bad-checksum.bin
BIOS_MB2_IMAGE := $(BIOS_MB2_DIR)/ours.iso
BIOS_MB2_A20_IMAGE := $(BIOS_MB2_DIR)/a20-skip.iso
BIOS_MB2_TRUNC_IMAGE := $(BIOS_MB2_DIR)/e820-truncated.iso
BIOS_MB2_BAD_RSDP_IMAGE := $(BIOS_MB2_DIR)/rsdp-bad-checksum.iso
BIOS_MB2_GRUB_IMAGE := $(BIOS_MB2_DIR)/grub-dump.iso
BIOS_MB2_GRUB_WORK_IMAGE := $(BIOS_MB2_DIR)/grub-dump-work.iso
BIOS_MB2_GRUB_KERNEL := $(BIOS_MB2_DIR)/kernel-dump.elf
BIOS_MB2_TEST := tests/unit/bios_mb2_test.py
BIOS_MB2_QEMU ?= qemu-system-x86_64

.PHONY: test-bios-mb2 test-bios-mb2-negctl \
    test-bios-mb2-differential test-bios-mb2-differential-negctl

$(BIOS_MB2_PRELOAD): c/boot/bios/preload.asm tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	nasm -f bin -o $@ $<

$(BIOS_MB2_LOADER): c/boot/bios/loader.asm tests/fixtures/bios/mb2-dump.asm tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	nasm -f bin -o $@ tests/fixtures/bios/mb2-dump.asm

$(BIOS_MB2_A20_LOADER): c/boot/bios/loader.asm tests/fixtures/bios/mb2-dump.asm tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	nasm -f bin -DLOADER_NEGCTL_SKIP_A20 -o $@ tests/fixtures/bios/mb2-dump.asm

$(BIOS_MB2_TRUNC_LOADER): c/boot/bios/loader.asm tests/fixtures/bios/mb2-dump.asm tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	# Two entries keep the control parseable and retain one usable region, while
	# the real nine-entry SeaBIOS map makes the missing suffix unambiguous.
	nasm -f bin -DLOADER_NEGCTL_TRUNCATE_E820=2 -o $@ tests/fixtures/bios/mb2-dump.asm

$(BIOS_MB2_BAD_RSDP_LOADER): c/boot/bios/loader.asm tests/fixtures/bios/mb2-dump.asm tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	nasm -f bin -DLOADER_NEGCTL_BAD_RSDP -o $@ tests/fixtures/bios/mb2-dump.asm

$(BIOS_MB2_IMAGE): tools/mkiso.py $(BIOS_MB2_PRELOAD) $(BIOS_MB2_LOADER) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_MB2_PRELOAD) --loader $(BIOS_MB2_LOADER)

$(BIOS_MB2_A20_IMAGE): tools/mkiso.py $(BIOS_MB2_PRELOAD) $(BIOS_MB2_A20_LOADER) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_MB2_PRELOAD) --loader $(BIOS_MB2_A20_LOADER)

$(BIOS_MB2_TRUNC_IMAGE): tools/mkiso.py $(BIOS_MB2_PRELOAD) $(BIOS_MB2_TRUNC_LOADER) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_MB2_PRELOAD) --loader $(BIOS_MB2_TRUNC_LOADER)

$(BIOS_MB2_BAD_RSDP_IMAGE): tools/mkiso.py $(BIOS_MB2_PRELOAD) $(BIOS_MB2_BAD_RSDP_LOADER) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_MB2_PRELOAD) --loader $(BIOS_MB2_BAD_RSDP_LOADER)

# Build the ordinary kernel objects in the caller's isolated BUILD tree,
# recompile only the two diagnostic translation units with BOOT_MB2_DUMP, then
# link a separately named kernel and ask the ordinary GRUB recipe for a
# separately named ISO.  Naming both outputs is important: merely recompiling
# two objects inside one filesystem timestamp tick once left the old kernel
# linked and the apparent "differential" ran no dumper at all.  The product ISO
# recipe, product ISO pathname, and grub.cfg remain exactly as shipped.
$(BIOS_MB2_GRUB_IMAGE): c/kernel/core/mb2dump.c c/kernel/core/kmain.c tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	$(MAKE) BUILD=$(BUILD) $(KERNEL)
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/mb2dump.c -o $(BUILD)/c/kernel/core/mb2dump.o
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/kmain.c -o $(BUILD)/c/kernel/core/kmain.o
	$(MAKE) BUILD=$(BUILD) KERNEL=$(BIOS_MB2_GRUB_KERNEL) $(BIOS_MB2_GRUB_KERNEL)
	$(MAKE) BUILD=$(BUILD) KERNEL=$(BIOS_MB2_GRUB_KERNEL) \
	    ISO=$(BIOS_MB2_GRUB_WORK_IMAGE) $(BIOS_MB2_GRUB_WORK_IMAGE)
	cp $(BIOS_MB2_GRUB_WORK_IMAGE) $@

# SeaBIOS on the measured host enters with A20 enabled.  The control therefore
# cannot demonstrate a failed enable request here; it still executes the alias
# write/read verification, and the oracle prints a loud SKIP naming the missing
# firmware state instead of laundering an unobservable control into a pass.
test-bios-mb2-negctl: $(BIOS_MB2_A20_IMAGE) $(BIOS_MB2_TEST)
	@if ! command -v $(BIOS_MB2_QEMU) >/dev/null 2>&1; then \
	  echo 'SKIP: test-bios-mb2-negctl requires $(BIOS_MB2_QEMU) to watch the A20 alias control'; \
	  exit 0; \
	 fi; \
	 python3 $(BIOS_MB2_TEST) --qemu $(BIOS_MB2_QEMU) a20-control $(BIOS_MB2_A20_IMAGE)

test-bios-mb2: test-bios-mb2-negctl $(BIOS_MB2_IMAGE) $(BIOS_MB2_TEST)
	@python3 $(BIOS_MB2_TEST) --qemu $(BIOS_MB2_QEMU) check $(BIOS_MB2_IMAGE)

# Both mutations remain valid blocks so the differential, not a parser crash,
# is what goes red.  The comparator prints every differing consumed field on
# adjacent CONTROL/GRUB lines; that is intentionally more verbose than a raw
# unified diff because memory-map suffix loss is otherwise easy to miss.
test-bios-mb2-differential-negctl: $(BIOS_MB2_TRUNC_IMAGE) \
    $(BIOS_MB2_BAD_RSDP_IMAGE) $(BIOS_MB2_GRUB_IMAGE) $(BIOS_MB2_TEST)
	@python3 $(BIOS_MB2_TEST) --qemu $(BIOS_MB2_QEMU) expect-difference \
	    $(BIOS_MB2_TRUNC_IMAGE) $(BIOS_MB2_GRUB_IMAGE) --reason truncated-e820
	@python3 $(BIOS_MB2_TEST) --qemu $(BIOS_MB2_QEMU) expect-difference \
	    $(BIOS_MB2_BAD_RSDP_IMAGE) $(BIOS_MB2_GRUB_IMAGE) --reason bad-rsdp-checksum

test-bios-mb2-differential: test-bios-mb2-differential-negctl \
    $(BIOS_MB2_IMAGE) $(BIOS_MB2_GRUB_IMAGE) $(BIOS_MB2_TEST)
	@python3 $(BIOS_MB2_TEST) --qemu $(BIOS_MB2_QEMU) compare \
	    $(BIOS_MB2_IMAGE) $(BIOS_MB2_GRUB_IMAGE)
