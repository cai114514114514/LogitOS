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


$(MKISO_FIXTURE): tests/fixtures/mkiso/serial-marker.asm tests/bootself.mk
	@mkdir -p $(MKISO_DIR)
	nasm -f bin -o $@ $<

$(MKISO_IMAGE): tools/mkiso.py $(MKISO_FIXTURE) tests/bootself.mk
	python3 tools/mkiso.py $@ --boot-image $(MKISO_FIXTURE)



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

