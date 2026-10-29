# SPDX-License-Identifier: MIT
# Isolated verification builds; ordinary kernels contain no rendezvous selector.
ifeq ($(BKLVERIFY),1)
CFLAGS += -DBKL_VERIFY
OBJ += $(BUILD)/tests/unit/bkl_guest_kernel.o
$(KERNEL): $(BUILD)/tests/unit/bkl_guest_kernel.o
endif
ifeq ($(BKLSERIAL),1)
CFLAGS += -DBKL_VERIFY_SERIAL
endif
ifeq ($(BKLIRQOFF),1)
CFLAGS += -DBKL_VERIFY_IRQ_OFF
endif
.PHONY: bkl-image bkl-disk
$(BUILD)/bkl.elf: tests/unit/bkl_guest.c c/apps/crt0_cli.asm c/apps/clib.h
	@mkdir -p $(BUILD)/tests/unit
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/tests/unit/bkl.crt.o
	$(CC) $(UCFLAGS) -Ic/apps -Itests/unit -c $< -o $(BUILD)/tests/unit/bkl.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/tests/unit/bkl.crt.o $(BUILD)/tests/unit/bkl.o
$(BUILD)/bkl.aex: $(BUILD)/bkl.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ bkl - '*' 150 150 150 --cli
bkl-image: $(ISO) $(BUILD)/esp.img $(BUILD)/bkl.aex
bkl-disk: $(BUILD)/bkl.aex $(BUILD)/sigtest.aex $(BUILD)/smptest.aex
	$(MAKE) -n -W tools/mkfs.py BUILD=$(WIDE_BASE_BUILD) $(WIDE_BASE_BUILD)/disk.img > $(BUILD)/bkl-disk.make
	python3 tests/boot/mk-bkl-disk.py $(BUILD)

# Runs its stages sequentially even when outer make uses -j. Reused product
# application binaries never cause browser/third-party source rebuilds.
BKL_BASELINE_BUILD ?=
BKL_BASELINE_TAR ?=

