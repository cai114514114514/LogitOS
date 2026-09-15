# SPDX-License-Identifier: MIT
# Ordinary allocation is unconditional. Only the pressure/stack test syscall
# is optional; all fixtures, controls and artifacts live in the caller BUILD.
ifeq ($(HIGHHEAPVERIFY),1)
CFLAGS += -DHIGHHEAP_VERIFY
OBJ += $(BUILD)/tests/unit/highheap_guest_kernel.o
$(KERNEL): $(BUILD)/tests/unit/highheap_guest_kernel.o
endif
ifneq ($(HIGHHEAP_CONTROL),)
# Separate output tree: control bytes cannot poison an ordinary object cache.
ifeq ($(HIGHHEAP_CONTROL),module-high)
HIGHHEAP_CONTROL_FILE := c/kernel/module/modload
else
HIGHHEAP_CONTROL_FILE := c/kernel/mm/kheap
endif
$(BUILD)/$(HIGHHEAP_CONTROL_FILE).o: $(HIGHHEAP_CONTROL_FILE).c tests/unit/highheap_variant.py
	python3 tests/unit/highheap_variant.py --out $(BUILD)/heap-control --control $(HIGHHEAP_CONTROL)
	$(CC) $(CFLAGS) -c $(BUILD)/heap-control/$(HIGHHEAP_CONTROL_FILE).c -o $@
endif
.PHONY: test-highheap-host test-highheap-negctl highheap-image highheap-disk test-highheap-all
test-highheap-host: test-highheap-negctl
test-highheap-negctl:
	python3 tests/unit/highheap_run.py --build $(BUILD)/core
	python3 tests/unit/highheap_panic_run.py --build $(BUILD)/panic
$(BUILD)/highheap.elf: tests/unit/highheap_guest.c tests/unit/highheap_verify.h c/apps/crt0_cli.asm c/apps/clib.h
	@mkdir -p $(BUILD)/tests/unit
	$(ASM) -f elf64 c/apps/crt0_cli.asm -o $(BUILD)/tests/unit/highheap.crt.o
	$(CC) $(UCFLAGS) -Ic/apps -c $< -o $(BUILD)/tests/unit/highheap.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/tests/unit/highheap.crt.o $(BUILD)/tests/unit/highheap.o
$(BUILD)/highheap.aex: $(BUILD)/highheap.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ highheap - '*' 150 150 150 --cli
$(BUILD)/highheap.ko: tests/unit/highheap_module.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
highheap-image: $(ISO) $(BUILD)/esp.img $(BUILD)/highheap.aex $(BUILD)/highheap.ko
highheap-disk: $(BUILD)/highheap.aex $(BUILD)/highheap.ko
	$(MAKE) -n -W tools/mkfs.py BUILD=$(WIDE_BASE_BUILD) $(WIDE_BASE_BUILD)/disk.img > $(BUILD)/highheap-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/highheap-disk.make $(BUILD)/highheap-disk.img $(BUILD)/highheap.aex:/bin/highheap $(BUILD)/highheap.ko:/lib/modules/highheap.ko
test-highheap-all:
	python3 tests/boot/run-highheap-acceptance.py --build $(BUILD) --apps $(WIDE_BASE_BUILD) $(if $(HIGHHEAP_APPS_DISK),--apps-disk $(HIGHHEAP_APPS_DISK)) $(if $(HIGHHEAP_BASELINE),--baseline $(HIGHHEAP_BASELINE))
