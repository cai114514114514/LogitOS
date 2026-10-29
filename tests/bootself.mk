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

# The native ABI has one C definition but two producer implementations.  UEFI
# gets compiler member offsets from logit_boot.h; BIOS gets numeric constants
# generated from the same header, then this source gate proves every hand-built
# field uses the symbol for the member it claims to write.  The kernel scan
# independently freezes the framing it accepts.  Controls mutate build-local
# copies because a deliberately wrong protocol header must never be includable
# by a product target.
BOOT_CONTRACT_DIR := $(BUILD)/boot-contract
BOOT_CONTRACT_TEST := tests/unit/boot_contract_test.py
BOOT_CONTRACT_BAD_HEADER := $(BOOT_CONTRACT_DIR)/logit_boot_bad_offset.h
BOOT_CONTRACT_BAD_ASM := $(BOOT_CONTRACT_DIR)/loader_bad_offset.asm
BOOT_CONTRACT_BAD_VERSION := $(BOOT_CONTRACT_DIR)/bootinfo_bad_version.c

.PHONY: test-boot-contract test-boot-contract-negctl

$(BOOT_CONTRACT_BAD_HEADER): include/abi/logit_boot.h tests/bootself.mk
	@mkdir -p $(BOOT_CONTRACT_DIR)
	@sed -e 's/uint16_t version;/uint16_t boot_contract_swap;/' \
	    -e 's/uint16_t header_size;/uint16_t version;/' \
	    -e 's/uint16_t boot_contract_swap;/uint16_t header_size;/' $< >$@

$(BOOT_CONTRACT_BAD_ASM): c/boot/bios/loader.asm tests/bootself.mk
	@mkdir -p $(BOOT_CONTRACT_DIR)
	@sed 's/LOGIT_BOOT_HEADER_VERSION_OFFSET\], LOGIT_BOOT_VERSION/LOGIT_BOOT_HEADER_HEADER_SIZE_OFFSET], LOGIT_BOOT_VERSION/' \
	    $< >$@

$(BOOT_CONTRACT_BAD_VERSION): c/kernel/init/bootinfo.c tests/bootself.mk
	@mkdir -p $(BOOT_CONTRACT_DIR)
	@sed 's/header->version != LOGIT_BOOT_VERSION/header->version != LOGIT_BOOT_VERSION + 1/' \
	    $< >$@

# Each mutation runs through the ordinary oracle and must emit the exact field
# or site plus both disagreeing values.  Merely observing a nonzero status would
# let a syntax error or missing input impersonate a working control.
test-boot-contract-negctl: $(BOOT_CONTRACT_TEST) $(BOOT_CONTRACT_BAD_HEADER) \
    $(BOOT_CONTRACT_BAD_ASM) $(BOOT_CONTRACT_BAD_VERSION)
	@set -e; failed=0; \
	 rc=0; python3 $(BOOT_CONTRACT_TEST) --header $(BOOT_CONTRACT_BAD_HEADER) \
	   >$(BOOT_CONTRACT_DIR)/bad-header.log 2>&1 || rc=$$?; \
	 cat $(BOOT_CONTRACT_DIR)/bad-header.log; \
	 if ! { test "$$rc" -eq 1 && grep -Fq 'FAIL: field header.version offset: C=6 BIOS=4' \
	   $(BOOT_CONTRACT_DIR)/bad-header.log; }; then \
	   echo 'test-boot-contract-negctl: FAIL -- moved-header-field control did not name C=6 BIOS=4'; failed=1; \
	 else echo 'PASS: moved-header-field control was watched failing'; fi; \
	 rc=0; python3 $(BOOT_CONTRACT_TEST) --asm $(BOOT_CONTRACT_BAD_ASM) \
	   >$(BOOT_CONTRACT_DIR)/bad-asm.log 2>&1 || rc=$$?; \
	 cat $(BOOT_CONTRACT_DIR)/bad-asm.log; \
	 if ! { test "$$rc" -eq 1 && grep -Fq 'FAIL: field header.version offset: C=4 BIOS=6' \
	   $(BOOT_CONTRACT_DIR)/bad-asm.log; }; then \
	   echo 'test-boot-contract-negctl: FAIL -- BIOS-offset control did not name C=4 BIOS=6'; failed=1; \
	 else echo 'PASS: BIOS-offset control was watched failing'; fi; \
	 rc=0; python3 $(BOOT_CONTRACT_TEST) --kernel $(BOOT_CONTRACT_BAD_VERSION) \
	   >$(BOOT_CONTRACT_DIR)/bad-version.log 2>&1 || rc=$$?; \
	 cat $(BOOT_CONTRACT_DIR)/bad-version.log; \
	 if ! { test "$$rc" -eq 1 && grep -Fq 'FAIL: protocol version kernel-check=0x0002 header=0x0001' \
	   $(BOOT_CONTRACT_DIR)/bad-version.log; }; then \
	   echo 'test-boot-contract-negctl: FAIL -- version control did not name 0x0002 and 0x0001'; failed=1; \
	 else echo 'PASS: protocol-version control was watched failing'; fi; \
	 test "$$failed" -eq 0 || exit 1; \
	 echo 'PASS: native boot-contract negative controls all failed as required'

test-boot-contract: test-boot-contract-negctl $(BOOT_CONTRACT_TEST) \
    include/abi/logit_boot.h c/boot/bios/loader.asm c/boot/efi/loader.c \
    c/kernel/init/bootinfo.c
	@python3 $(BOOT_CONTRACT_TEST)

# Shipping and test loaders now speak only native v1. The sole retired-protocol
# entry is assembled from tests/fixtures/bootoracle into a separately named
# GRUB kernel. That keeps an independent loader oracle without putting the old
# entry or scanned header in a product artifact. It can compare the fixed QEMU
# machine, but cannot catch QEMU/SeaBIOS changes or real-firmware differences.
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
BIOS_NATIVE_PRELOAD := $(BIOS_PRELOAD_BIN)
BIOS_BOOT_DISK := $(DISK)
BIOS_BOOT_TEST := tests/unit/bios_mb2_test.py
BIOS_BOOT_QEMU ?= qemu-system-x86_64
BIOS_ORACLE_DIR := $(BUILD)/bios-mb2
BIOS_ORACLE_BOOT_OBJ := $(BUILD)/tests/fixtures/bootoracle/boot.o
BIOS_ORACLE_HEADER_OBJ := $(BUILD)/tests/fixtures/bootoracle/multiboot2.o
BIOS_ORACLE_KERNEL := $(BIOS_ORACLE_DIR)/kernel-dump.elf
BIOS_ORACLE_IMAGE := $(BIOS_ORACLE_DIR)/grub-dump.iso
BIOS_ORACLE_WORK_IMAGE := $(BIOS_ORACLE_DIR)/grub-dump-work.iso

.PHONY: test-bios-native test-bios-native-negctl

# Same generator as the root Makefile's product rule -- see the comment there
# for what having two of them cost on 2026-09-15.
$(BIOS_NATIVE_ABI_INC): include/abi/logit_boot.h tools/gen_boot_inc.py tests/bootself.mk
	@mkdir -p $(BIOS_NATIVE_DIR)
	@python3 tools/gen_boot_inc.py $< -o $@

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
$(BIOS_NATIVE_DUMP_KERNEL): $(OBJ) $(RUST_LIB) linker.ld c/kernel/init/mb2dump.c \
    c/kernel/init/kmain.c c/kernel/init/bootinfo.c c/boot/long.asm \
    include/abi/logit_boot.h tests/bootself.mk
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/init/mb2dump.c -o $(BUILD)/c/kernel/init/mb2dump.o
	$(CC) $(CFLAGS) -DBOOT_MB2_DUMP -c c/kernel/init/kmain.c -o $(BUILD)/c/kernel/init/kmain.o
	$(LD) $(LDFLAGS) -e logit_native_start -Map=$(BIOS_NATIVE_DIR)/kernel-dump.map \
	    -o $@ --start-group $(OBJ) $(RUST_LIB) --end-group

$(BIOS_NATIVE_KERNEL_STAMP): $(BIOS_NATIVE_DUMP_IMAGE) c/kernel/init/mb2dump.c \
    c/kernel/init/kmain.c tests/bootself.mk
	$(CC) $(CFLAGS) -c c/kernel/init/mb2dump.c -o $(BUILD)/c/kernel/init/mb2dump.o
	$(CC) $(CFLAGS) -c c/kernel/init/kmain.c -o $(BUILD)/c/kernel/init/kmain.o
	$(LD) $(LDFLAGS) -e logit_native_start -Map=$(BIOS_NATIVE_DIR)/kernel.map \
	    -o $(BIOS_NATIVE_KERNEL) --start-group $(OBJ) $(RUST_LIB) --end-group
	@touch $@

$(BIOS_NATIVE_DUMP_IMAGE): tools/mkiso.py $(BIOS_NATIVE_PRELOAD) \
    $(BIOS_NATIVE_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_NATIVE_PRELOAD) \
	    --loader $(BIOS_NATIVE_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_IMAGE): tools/mkiso.py $(BIOS_NATIVE_PRELOAD) $(BIOS_NATIVE_LOADER) \
    $(BIOS_NATIVE_KERNEL_STAMP) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_NATIVE_PRELOAD) \
	    --loader $(BIOS_NATIVE_LOADER) --kernel $(BIOS_NATIVE_KERNEL)

$(BIOS_NATIVE_BAD_VERSION_IMAGE): tools/mkiso.py $(BIOS_NATIVE_PRELOAD) \
    $(BIOS_NATIVE_BAD_VERSION_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_NATIVE_PRELOAD) \
	    --loader $(BIOS_NATIVE_BAD_VERSION_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_TRUNCATED_IMAGE): tools/mkiso.py $(BIOS_NATIVE_PRELOAD) \
    $(BIOS_NATIVE_TRUNCATED_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_NATIVE_PRELOAD) \
	    --loader $(BIOS_NATIVE_TRUNCATED_LOADER) --kernel $(BIOS_NATIVE_DUMP_KERNEL)

$(BIOS_NATIVE_SHORT_MAP_IMAGE): tools/mkiso.py $(BIOS_NATIVE_PRELOAD) \
    $(BIOS_NATIVE_SHORT_MAP_LOADER) $(BIOS_NATIVE_DUMP_KERNEL) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(BIOS_NATIVE_PRELOAD) \
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

# The oracle reuses the same diagnostic consumers but adds the retired header
# and 32-bit climb only to this link. The iso-grub escape-hatch recipe packages
# it, so GRUB remains the independent producer rather than a native mirror.
$(BIOS_ORACLE_KERNEL): $(BIOS_NATIVE_DUMP_KERNEL) $(BIOS_ORACLE_BOOT_OBJ) \
    $(BIOS_ORACLE_HEADER_OBJ) tests/bootself.mk
	@mkdir -p $(BIOS_ORACLE_DIR)
	$(LD) $(LDFLAGS) -e start -Map=$(BIOS_ORACLE_DIR)/kernel-dump.map -o $@ \
	    --start-group $(OBJ) $(RUST_LIB) $(BIOS_ORACLE_BOOT_OBJ) \
	    $(BIOS_ORACLE_HEADER_OBJ) --end-group

$(BIOS_ORACLE_IMAGE): $(BIOS_ORACLE_KERNEL) grub.cfg tests/bootself.mk
	$(MAKE) BUILD=$(BUILD) GRUB_INPUT_KERNEL=$(BIOS_ORACLE_KERNEL) \
	    GRUB_ISO=$(BIOS_ORACLE_WORK_IMAGE) iso-grub
	cp $(BIOS_ORACLE_WORK_IMAGE) $@

test-bios-native: test-bios-native-negctl $(BIOS_NATIVE_DUMP_IMAGE) \
    $(BIOS_ORACLE_IMAGE) $(BIOS_NATIVE_IMAGE) $(BIOS_BOOT_TEST)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) compare \
	    $(BIOS_NATIVE_DUMP_IMAGE) $(BIOS_ORACLE_IMAGE)
	@python3 $(BIOS_BOOT_TEST) --qemu $(BIOS_BOOT_QEMU) --disk $(BIOS_BOOT_DISK) \
	    native-kernel-check $(BIOS_NATIVE_IMAGE)
