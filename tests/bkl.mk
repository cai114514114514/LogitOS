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
.PHONY: test-bkl-host test-bkl-mm-host test-bkl-tlb-host bkl-image bkl-disk
test-bkl-mm-host:
	python3 tests/unit/bkl_kernel_map_run.py --build $(BUILD)/bkl-kernel-map
	python3 tests/unit/bkl_mm_run.py --build $(BUILD)/bkl-mm
test-bkl-tlb-host:
	python3 tests/unit/bkl_tlb_run.py --build $(BUILD)/bkl-tlb
test-bkl-host: test-bkl-mm-host test-bkl-io-host test-bkl-tlb-host test-bkl-kbench-host test-serial-process-cleanup test-usb-process-cleanup
	python3 tests/unit/bkl_proc_run.py --build $(BUILD)/bkl-proc
	python3 tests/unit/bkl_gui_test.py --build $(BUILD)/bkl-gui
	python3 tests/unit/bkl_module_test.py --build $(BUILD)/bkl-module
	python3 tests/unit/bkl_console_test.py --build $(BUILD)/bkl-console
	python3 tests/unit/bkl_printf_test.py --build $(BUILD)/bkl-printf
	python3 tests/unit/bkl_device_test.py --build $(BUILD)/bkl-device
	python3 tests/unit/bkl_power_test.py --build $(BUILD)/bkl-power
	python3 tests/unit/bkl_kheap_run.py --build $(BUILD)/bkl-kheap
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
.PHONY: test-bkl-all
BKL_BASELINE_BUILD ?=
BKL_BASELINE_TAR ?=
test-bkl-all:
	python3 tests/boot/run-bkl-acceptance.py --build $(BUILD) --apps $(WIDE_BASE_BUILD) $(if $(BKL_BASELINE_BUILD),--baseline-build $(BKL_BASELINE_BUILD)) $(if $(BKL_BASELINE_TAR),--baseline-tar $(BKL_BASELINE_TAR))

.PHONY: test-bkl-kbench-host
test-bkl-kbench-host:
	python3 tests/unit/bkl_kbench_run.py --build $(BUILD)/bkl-kbench
