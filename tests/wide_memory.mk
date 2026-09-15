# SPDX-License-Identifier: MIT
# Independent opt-in image: production kernels expose no verification selector.
ifeq ($(WIDEVERIFY),1)
CFLAGS += -DMM_WIDE_VERIFY
OBJ += $(BUILD)/tests/unit/wide_memory_guest_kernel.o
$(KERNEL): $(BUILD)/tests/unit/wide_memory_guest_kernel.o
endif
ifeq ($(WIDENOMAP),1)
CFLAGS += -DPHYS_MAP_DISABLE_HIGH
endif
ifeq ($(WIDENOCLONE),1)
CFLAGS += -DWIDEVA_SKIP_CLONE
endif

$(BUILD)/wide-memory.elf: tests/unit/wide_memory_guest.c tests/unit/wide_memory_verify.h c/apps/crt0_cli.asm c/apps/clib.h
	@mkdir -p $(BUILD)/tests/unit
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/tests/unit/wide-memory.crt.o
	$(CC) $(UCFLAGS) -Ic/apps -Itests/unit -c $< -o $(BUILD)/tests/unit/wide-memory.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/tests/unit/wide-memory.crt.o $(BUILD)/tests/unit/wide-memory.o
$(BUILD)/wide-memory.aex: $(BUILD)/wide-memory.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ wide-memory - '*' 150 150 150 --cli

.PHONY: test-wideva test-wideva-negctl
# The runner executes the real fault/COW/rmap/swap stack and requires its
# omitted-high-subtree control to fail the named clone assertions.
test-wideva: test-wideva-negctl
test-wideva-negctl:
	python3 tests/unit/wideva_run.py --build $(BUILD)/wideva

.PHONY: test-physmap test-physmap-negctl
test-physmap: test-physmap-negctl
test-physmap-negctl:
	python3 tests/unit/physmap_test.py --root . --build $(BUILD)/physmap

# Reuse already-built product applications without rebuilding browser/third-party.
# The disk recipe is read from make itself, not maintained as a second file list.
WIDE_BASE_BUILD ?= build
.PHONY: wide-memory-image wide-memory-disk test-wide-memory-os test-kernel-disk-recipe
.PHONY: test-wide-dma-snapshot
test-wide-dma-snapshot:
	@mkdir -p $(BUILD)
	cc -std=c11 -Wall -Wextra -Werror -Ic/drivers/core -Itests/unit tests/unit/wide_dma_snapshot_test.c -o $(BUILD)/wide-dma-snapshot-test
	$(BUILD)/wide-dma-snapshot-test
test-kernel-disk-recipe:
	python3 tests/unit/make_disk_recipe_test.py
wide-memory-image: test-wide-dma-snapshot $(ISO) $(BUILD)/esp.img $(BUILD)/wide-memory.aex pie-fixtures
wide-memory-disk: test-kernel-disk-recipe $(BUILD)/wide-memory.aex pie-fixtures
	$(MAKE) -n -W tools/mkfs.py BUILD=$(WIDE_BASE_BUILD) $(WIDE_BASE_BUILD)/disk.img > $(BUILD)/wide-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/wide-disk.make $(BUILD)/wide-disk.img $(BUILD)/wide-memory.aex:/bin/wide-memory $(BUILD)/pie/program.elf:/bin/pie $(BUILD)/pie/program.aex:/bin/pie-aex
test-wide-memory-os: test-physmap test-wideva test-pie wide-memory-image wide-memory-disk
	@test "$(WIDEVERIFY)" = "1" || { echo "Use WIDEVERIFY=1 and an independent BUILD directory"; exit 1; }
	python3 tests/boot/run-wide-memory.py --build $(BUILD) --disk $(BUILD)/wide-disk.img --out $(BUILD)/wide-results
