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
# link a separately named kernel and ask the retained GRUB escape hatch for a
# separately named ISO.  Naming both outputs is important: merely recompiling
# two objects inside one filesystem timestamp tick once left the old kernel
# linked and the apparent "differential" ran no dumper at all.
# Correction (2026-09-15): the product ISO now uses mkiso.py; this differential
# deliberately keeps its oracle on iso-grub for one release so a loader defect
# cannot make both sides of the comparison agree on the same wrong behavior.
$(BIOS_MB2_GRUB_IMAGE): c/kernel/core/mb2dump.c c/kernel/core/kmain.c tests/bootself.mk
	@mkdir -p $(BIOS_MB2_DIR)
	$(MAKE) BUILD=$(BUILD) $(KERNEL)
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/mb2dump.c -o $(BUILD)/c/kernel/core/mb2dump.o
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/kmain.c -o $(BUILD)/c/kernel/core/kmain.o
	$(MAKE) BUILD=$(BUILD) KERNEL=$(BIOS_MB2_GRUB_KERNEL) $(BIOS_MB2_GRUB_KERNEL)
	$(MAKE) BUILD=$(BUILD) KERNEL=$(BIOS_MB2_GRUB_KERNEL) \
	    GRUB_ISO=$(BIOS_MB2_GRUB_WORK_IMAGE) iso-grub
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

# Stage 3 kept the product ISO and grub.cfg untouched. Correction (2026-09-15):
# the product now uses this preload/loader/kernel layout; the sibling image here
# remains separately named. The GRUB image above is still built because its
# BOOT_MB2_DUMP is the oracle for consumed tags, not because GRUB participates
# in our product boot.
BIOS_BOOT_DIR := $(BUILD)/bios-boot
BIOS_BOOT_PRELOAD := $(BIOS_BOOT_DIR)/preload.bin
BIOS_BOOT_LOADER := $(BIOS_BOOT_DIR)/loader.bin
BIOS_BOOT_BAD_MAGIC_LOADER := $(BIOS_BOOT_DIR)/loader-bad-magic.bin
BIOS_BOOT_SHORT_SEGMENT_LOADER := $(BIOS_BOOT_DIR)/loader-short-segment.bin
BIOS_BOOT_SKIP_BSS_LOADER := $(BIOS_BOOT_DIR)/loader-skip-bss-zero.bin
BIOS_BOOT_IMAGE := $(BIOS_BOOT_DIR)/ours.iso
BIOS_BOOT_BAD_MAGIC_IMAGE := $(BIOS_BOOT_DIR)/bad-magic.iso
BIOS_BOOT_SHORT_SEGMENT_IMAGE := $(BIOS_BOOT_DIR)/short-segment.iso
BIOS_BOOT_SKIP_BSS_IMAGE := $(BIOS_BOOT_DIR)/skip-bss-zero.iso
BIOS_BOOT_DUMP_IMAGE := $(BIOS_BOOT_DIR)/ours-dump.iso
BIOS_BOOT_KERNEL := $(BIOS_BOOT_DIR)/kernel.elf
BIOS_BOOT_KERNEL_STAMP := $(BIOS_BOOT_DIR)/kernel-normal.stamp
BIOS_BOOT_DUMP_KERNEL := $(BIOS_MB2_GRUB_KERNEL)
BIOS_BOOT_GRUB_IMAGE := $(BIOS_MB2_GRUB_IMAGE)
BIOS_BOOT_DISK := $(BUILD)/disk.img
BIOS_BOOT_TEST := tests/unit/bios_mb2_test.py
BIOS_BOOT_QEMU ?= qemu-system-x86_64

.PHONY: test-bios-boot test-bios-boot-negctl

$(BIOS_BOOT_PRELOAD): c/boot/bios/preload.asm tests/bootself.mk
	@mkdir -p $(BIOS_BOOT_DIR)
	nasm -f bin -o $@ $<

$(BIOS_BOOT_LOADER): c/boot/bios/loader.asm tests/bootself.mk
	@mkdir -p $(BIOS_BOOT_DIR)
	nasm -f bin -o $@ $<

$(BIOS_BOOT_BAD_MAGIC_LOADER): c/boot/bios/loader.asm tests/bootself.mk
	@mkdir -p $(BIOS_BOOT_DIR)
	nasm -f bin -DLOADER_NEGCTL_BAD_MB2_MAGIC -o $@ $<

$(BIOS_BOOT_SHORT_SEGMENT_LOADER): c/boot/bios/loader.asm tests/bootself.mk
	@mkdir -p $(BIOS_BOOT_DIR)
	nasm -f bin -DLOADER_NEGCTL_SHORT_SEGMENT -o $@ $<

$(BIOS_BOOT_SKIP_BSS_LOADER): c/boot/bios/loader.asm tests/bootself.mk
	@mkdir -p $(BIOS_BOOT_DIR)
	nasm -f bin -DLOADER_NEGCTL_SKIP_BSS_ZERO -o $@ $<

# The differential recipe deliberately leaves mb2dump.o and kmain.o compiled
# with BOOT_MB2_DUMP.  Recompile those two without it before linking the normal
# sibling kernel; otherwise an incremental run can silently call a dump-and-
# exit artifact "normal" and make LOGIT_BOOT_OK impossible by construction.
$(BIOS_BOOT_KERNEL_STAMP): $(BIOS_BOOT_GRUB_IMAGE) c/kernel/core/mb2dump.c \
    c/kernel/core/kmain.c tests/bootself.mk
	$(CC) $(CFLAGS) -c c/kernel/core/mb2dump.c -o $(BUILD)/c/kernel/core/mb2dump.o
	$(CC) $(CFLAGS) -c c/kernel/core/kmain.c -o $(BUILD)/c/kernel/core/kmain.o
	rm -f $(BIOS_BOOT_KERNEL)
	$(MAKE) BUILD=$(BUILD) KERNEL=$(BIOS_BOOT_KERNEL) $(BIOS_BOOT_KERNEL)
	@touch $@

$(BIOS_BOOT_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) $(BIOS_BOOT_LOADER) \
    $(BIOS_BOOT_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_BOOT_LOADER) --kernel $(BIOS_BOOT_KERNEL)

$(BIOS_BOOT_BAD_MAGIC_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_BOOT_BAD_MAGIC_LOADER) $(BIOS_BOOT_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_BOOT_BAD_MAGIC_LOADER) --kernel $(BIOS_BOOT_KERNEL)

$(BIOS_BOOT_SHORT_SEGMENT_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_BOOT_SHORT_SEGMENT_LOADER) $(BIOS_BOOT_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_BOOT_SHORT_SEGMENT_LOADER) --kernel $(BIOS_BOOT_KERNEL)

$(BIOS_BOOT_SKIP_BSS_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_BOOT_SKIP_BSS_LOADER) $(BIOS_BOOT_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_BOOT_SKIP_BSS_LOADER) --kernel $(BIOS_BOOT_KERNEL)

$(BIOS_BOOT_DUMP_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) $(BIOS_BOOT_LOADER) \
    $(BIOS_BOOT_GRUB_IMAGE) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_BOOT_LOADER) --kernel $(BIOS_BOOT_DUMP_KERNEL)

# These mutations are prerequisites of the positive gate.  The BSS half is
# allowed to SKIP only after the serial transcript proves zeroing was omitted
# and entry was attempted: zero-filled emulator RAM can make the defect
# unobservable, and treating that luck as a failed control would be fabricated
# evidence.  The wrong-magic and short-segment halves must fail on every run.
test-bios-boot-negctl: $(BIOS_BOOT_BAD_MAGIC_IMAGE) \
    $(BIOS_BOOT_SHORT_SEGMENT_IMAGE) $(BIOS_BOOT_SKIP_BSS_IMAGE) \
    $(BIOS_BOOT_DISK) $(BIOS_BOOT_TEST)
	@if ! command -v $(BIOS_BOOT_QEMU) >/dev/null 2>&1; then \
	  echo 'SKIP: test-bios-boot-negctl requires $(BIOS_BOOT_QEMU) to watch the kernel-entry controls'; \
	  exit 0; \
	 fi
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) kernel-control \
	    $(BIOS_BOOT_BAD_MAGIC_IMAGE) --reason bad-magic
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) kernel-control \
	    $(BIOS_BOOT_SHORT_SEGMENT_IMAGE) --reason short-segment
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) kernel-control \
	    $(BIOS_BOOT_SKIP_BSS_IMAGE) --reason skip-bss-zero
	@echo 'PASS: bios-boot negative controls completed with any unobservable half skipped loudly'

test-bios-boot: test-bios-boot-negctl $(BIOS_BOOT_IMAGE) \
    $(BIOS_BOOT_DUMP_IMAGE) $(BIOS_BOOT_TEST)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) kernel-compare \
	    $(BIOS_BOOT_DUMP_IMAGE) $(BIOS_BOOT_GRUB_IMAGE)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) kernel-check \
	    $(BIOS_BOOT_IMAGE)

# Phase 2 keeps the MB2 artifacts above as both compatibility path and oracle.
# Only these native targets relink the same kernel with logit_native_start as
# ELF e_entry; there is no scanned header and no address baked into the loader.
# The NASM include is mechanically derived from the C ABI header so protocol
# numbers have one authoritative spelling.
BIOS_NATIVE_DIR := $(BUILD)/bios-native
BIOS_NATIVE_ABI_INC := $(BIOS_NATIVE_DIR)/logit_boot.inc
BIOS_NATIVE_LOADER := $(BIOS_NATIVE_DIR)/loader.bin
BIOS_NATIVE_BAD_VERSION_LOADER := $(BIOS_NATIVE_DIR)/loader-bad-version.bin
BIOS_NATIVE_TRUNCATED_LOADER := $(BIOS_NATIVE_DIR)/loader-truncated.bin
BIOS_NATIVE_SHORT_MAP_LOADER := $(BIOS_NATIVE_DIR)/loader-short-map.bin
BIOS_NATIVE_DUMP_KERNEL := $(BIOS_NATIVE_DIR)/kernel-dump.elf
BIOS_NATIVE_KERNEL := $(BIOS_NATIVE_DIR)/kernel.elf
BIOS_NATIVE_KERNEL_STAMP := $(BIOS_NATIVE_DIR)/kernel-normal.stamp
BIOS_NATIVE_DUMP_IMAGE := $(BIOS_NATIVE_DIR)/native-dump.iso
BIOS_NATIVE_IMAGE := $(BIOS_NATIVE_DIR)/native.iso
BIOS_NATIVE_BAD_VERSION_IMAGE := $(BIOS_NATIVE_DIR)/bad-version.iso
BIOS_NATIVE_TRUNCATED_IMAGE := $(BIOS_NATIVE_DIR)/truncated.iso
BIOS_NATIVE_SHORT_MAP_IMAGE := $(BIOS_NATIVE_DIR)/short-map.iso

.PHONY: test-bios-native test-bios-native-negctl

$(BIOS_NATIVE_ABI_INC): include/abi/logit_boot.h tests/bootself.mk
	@mkdir -p $(BIOS_NATIVE_DIR)
	@awk '/^#define LOGIT_BOOT_(MAGIC|VERSION|HEADER_SIZE|IDENTITY_MAP_BYTES|IDENTITY_PAGE_BYTES|BASE_PAGE_BYTES|TAG_)/ { print "%define " $$2 " " $$3 }' $< >$@

$(BIOS_NATIVE_LOADER): c/boot/bios/loader.asm $(BIOS_NATIVE_ABI_INC) tests/bootself.mk
	nasm -f bin -DLOADER_NATIVE -I$(BIOS_NATIVE_DIR)/ -o $@ $<

$(BIOS_NATIVE_BAD_VERSION_LOADER): c/boot/bios/loader.asm $(BIOS_NATIVE_ABI_INC) tests/bootself.mk
	nasm -f bin -DLOADER_NATIVE -DLOADER_NEGCTL_NATIVE_BAD_VERSION \
	    -I$(BIOS_NATIVE_DIR)/ -o $@ $<

$(BIOS_NATIVE_TRUNCATED_LOADER): c/boot/bios/loader.asm $(BIOS_NATIVE_ABI_INC) tests/bootself.mk
	nasm -f bin -DLOADER_NATIVE -DLOADER_NEGCTL_NATIVE_TRUNCATED \
	    -I$(BIOS_NATIVE_DIR)/ -o $@ $<

$(BIOS_NATIVE_SHORT_MAP_LOADER): c/boot/bios/loader.asm $(BIOS_NATIVE_ABI_INC) tests/bootself.mk
	nasm -f bin -DLOADER_NATIVE -DLOADER_NEGCTL_NATIVE_SHORT_MAP \
	    -I$(BIOS_NATIVE_DIR)/ -o $@ $<

# Build the dump sibling after explicitly installing dump objects.  The normal
# sibling reverses those two objects before relinking, preserving the same
# apparatus guard already paid for by test-bios-boot above.
$(BIOS_NATIVE_DUMP_KERNEL): $(OBJ) $(RUST_LIB) linker.ld c/kernel/core/mb2dump.c \
    c/kernel/core/kmain.c c/kernel/core/bootinfo.c c/boot/long.asm \
    include/abi/logit_boot.h tests/bootself.mk
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/mb2dump.c -o $(BUILD)/c/kernel/core/mb2dump.o
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/core/kmain.c -o $(BUILD)/c/kernel/core/kmain.o
	$(LD) $(LDFLAGS) -e logit_native_start -Map=$(BIOS_NATIVE_DIR)/kernel-dump.map \
	    -o $@ --start-group $(OBJ) $(RUST_LIB) --end-group

$(BIOS_NATIVE_KERNEL_STAMP): $(BIOS_NATIVE_DUMP_IMAGE) c/kernel/core/mb2dump.c \
    c/kernel/core/kmain.c tests/bootself.mk
	$(CC) $(CFLAGS) -c c/kernel/core/mb2dump.c -o $(BUILD)/c/kernel/core/mb2dump.o
	$(CC) $(CFLAGS) -c c/kernel/core/kmain.c -o $(BUILD)/c/kernel/core/kmain.o
	$(LD) $(LDFLAGS) -e logit_native_start -Map=$(BIOS_NATIVE_DIR)/kernel.map \
	    -o $(BIOS_NATIVE_KERNEL) --start-group $(OBJ) $(RUST_LIB) --end-group
	@touch $@

$(BIOS_NATIVE_DUMP_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_NATIVE_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_NATIVE_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) $(BIOS_NATIVE_LOADER) \
    $(BIOS_NATIVE_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_NATIVE_LOADER) --kernel $(BIOS_NATIVE_KERNEL)

$(BIOS_NATIVE_BAD_VERSION_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_NATIVE_BAD_VERSION_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_NATIVE_BAD_VERSION_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_TRUNCATED_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_NATIVE_TRUNCATED_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_NATIVE_TRUNCATED_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_SHORT_MAP_IMAGE): tools/mkiso.py $(BIOS_BOOT_PRELOAD) \
    $(BIOS_NATIVE_SHORT_MAP_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_BOOT_PRELOAD) \
	    --loader $(BIOS_NATIVE_SHORT_MAP_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

# All three controls are prerequisites.  The map control's failure is the
# absent final 4 KiB PTE itself: the kernel prints the promised-extent probe,
# touches its final byte, and must fault before LOGIT_BOOT_NATIVE_OK.
test-bios-native-negctl: $(BIOS_NATIVE_BAD_VERSION_IMAGE) \
    $(BIOS_NATIVE_TRUNCATED_IMAGE) $(BIOS_NATIVE_SHORT_MAP_IMAGE) $(BIOS_BOOT_DISK) \
    $(BIOS_BOOT_TEST)
	@if ! command -v $(BIOS_BOOT_QEMU) >/dev/null 2>&1; then \
	  echo 'SKIP: test-bios-native-negctl requires $(BIOS_BOOT_QEMU) to watch the native-entry controls'; \
	  exit 0; \
	 fi
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) \
	    native-control $(BIOS_NATIVE_BAD_VERSION_IMAGE) --reason bad-version
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) \
	    native-control $(BIOS_NATIVE_TRUNCATED_IMAGE) --reason truncated-tags
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) \
	    native-control $(BIOS_NATIVE_SHORT_MAP_IMAGE) --reason short-map
	@echo 'PASS: bios-native negative controls all failed as required'

test-bios-native: test-bios-native-negctl $(BIOS_NATIVE_DUMP_IMAGE) \
    $(BIOS_BOOT_DUMP_IMAGE) $(BIOS_MB2_GRUB_IMAGE) $(BIOS_NATIVE_IMAGE) $(BIOS_BOOT_TEST)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) three-way \
	    $(BIOS_NATIVE_DUMP_IMAGE) $(BIOS_BOOT_DUMP_IMAGE) $(BIOS_MB2_GRUB_IMAGE)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) \
	    native-kernel-check $(BIOS_NATIVE_IMAGE)
