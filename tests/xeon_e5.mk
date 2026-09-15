# LGA2011-0 Sandy/Ivy Bridge-EP CPU acceptance.
XEON_BASE_BUILD ?= build-driver-integration

ifeq ($(XEON_CPU_CAP_NEGCTL),1)
CFLAGS += -DLOGIT_CPU_CAP_NEGCTL
endif
ifeq ($(XEON_OSXSAVE_NEGCTL),1)
ASFLAGS += -DLOGIT_OSXSAVE_NEGCTL
endif

XEON_HOST_BIN := $(BUILD)/xeon-e5-platform-test
XEON_LOCK_DIAG_BIN := $(BUILD)/xeon-e5-lock-diag-test
$(XEON_HOST_BIN): tests/unit/xeon_e5_platform_test.c c/kernel/cpu/cpu_platform.c c/kernel/cpu/cpu_platform.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) tests/unit/xeon_e5_platform_test.c c/kernel/cpu/cpu_platform.c -o $@

$(XEON_LOCK_DIAG_BIN): tests/unit/xeon_e5_lock_diag_test.c c/kernel/cpu/spinlock.c c/kernel/cpu/spinlock.h
	@mkdir -p $(dir $@)
	$(CC) -std=c11 -O2 -Wall -Wextra -Werror -DLOGIT_LOCK_HOST $(KCPU_INC) tests/unit/xeon_e5_lock_diag_test.c c/kernel/cpu/spinlock.c -o $@

.PHONY: test-xeon-e5-host test-xeon-e5-platform-control
test-xeon-e5-host: test-xeon-e5-platform-control $(XEON_HOST_BIN) $(XEON_LOCK_DIAG_BIN)
	$(XEON_HOST_BIN)
	$(XEON_LOCK_DIAG_BIN)
	python3 tests/unit/xeon_e5_policy.py --negative-osxsave --negative-cpucount --negative-lockdiag

test-xeon-e5-platform-control:
	@mkdir -p $(BUILD)/xeon-e5-control
	@python3 tests/unit/xeon_e5_policy.py --write-platform-negctl $(BUILD)/xeon-e5-control/cpu_platform.c
	@$(CC) -std=c11 -O2 -Wall -Wextra -Werror $(KCPU_INC) tests/unit/xeon_e5_platform_test.c $(BUILD)/xeon-e5-control/cpu_platform.c -o $(BUILD)/xeon-e5-control/platform-negctl
	@if $(BUILD)/xeon-e5-control/platform-negctl > $(BUILD)/xeon-e5-control/platform-negctl.log 2>&1; then cat $(BUILD)/xeon-e5-control/platform-negctl.log; echo "NEGCTL FAIL: ignoring CPUID.0B topology still passed"; exit 1; else echo "NEGCTL RED: ignoring CPUID.0B topology was rejected"; fi

$(BUILD)/xeonobj/tests/unit/xeon_e5_guest.o: tests/unit/xeon_e5_guest.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/xeon-e5-check.elf: $(BUILD)/xeonobj/tests/unit/xeon_e5_guest.o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm c/apps/libc/logit_tls.ld
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/xeon-e5-check.crt0.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -T c/apps/libc/logit_tls.ld -o $@ $(BUILD)/apps/xeon-e5-check.crt0.o $(BUILD)/xeonobj/tests/unit/xeon_e5_guest.o $(LIBC_OBJS)

$(BUILD)/xeon-e5-check.aex: $(BUILD)/xeon-e5-check.elf tools/mkaex.py
	python3 tools/mkaex.py $< $@ xeon-e5-check - '*' 150 150 150 --cli

.PHONY: xeon-e5-image xeon-e5-disk
xeon-e5-image: $(ISO) $(BUILD)/esp.img

xeon-e5-disk: $(BUILD)/xeon-e5-check.aex
	@if [ ! -f $(XEON_BASE_BUILD)/disk.img ]; then $(MAKE) --no-print-directory BUILD=$(XEON_BASE_BUILD) $(XEON_BASE_BUILD)/disk.img; fi
	$(MAKE) -n -W tools/mkfs.py BUILD=$(XEON_BASE_BUILD) $(XEON_BASE_BUILD)/disk.img > $(BUILD)/xeon-e5-disk.make
	python3 tests/boot/mk-tcc-disk.py . $(BUILD)/xeon-e5-disk.make $(BUILD)/xeon-e5-disk.img $(BUILD)/xeon-e5-check.aex:/bin/xeon-e5-check

.PHONY: test-xeon-e5-bios test-xeon-e5-uefi
test-xeon-e5-bios: test-xeon-e5-host xeon-e5-image xeon-e5-disk
	python3 tests/boot/run-xeon-e5.py --build $(BUILD) --disk $(BUILD)/xeon-e5-disk.img --firmware bios --cpu SandyBridge-v1,model=45 --smp 16 --model 0x2d --generation sandy-bridge-ep --out $(BUILD)/xeon-e5-runs/bios-16

test-xeon-e5-uefi: test-xeon-e5-host xeon-e5-image xeon-e5-disk
	python3 tests/boot/run-xeon-e5.py --build $(BUILD) --disk $(BUILD)/xeon-e5-disk.img --firmware uefi --cpu IvyBridge-v1,model=62 --smp 24 --model 0x3e --generation ivy-bridge-ep --out $(BUILD)/xeon-e5-runs/uefi-24

.PHONY: test-xeon-e5-cap-control test-xeon-e5-osxsave-control
test-xeon-e5-cap-control: test-xeon-e5-host xeon-e5-disk
	$(MAKE) --no-print-directory BUILD=$(BUILD)-cap-neg XEON_CPU_CAP_NEGCTL=1 xeon-e5-image
	python3 tests/boot/run-xeon-e5.py --build $(BUILD)-cap-neg --disk $(BUILD)/xeon-e5-disk.img --firmware bios --cpu IvyBridge-v1,model=62 --smp 24 --model 0x3e --generation ivy-bridge-ep --expect-cap-failure --out $(BUILD)/xeon-e5-runs/cap-neg

test-xeon-e5-osxsave-control: test-xeon-e5-host xeon-e5-disk
	$(MAKE) --no-print-directory BUILD=$(BUILD)-osxsave-neg XEON_OSXSAVE_NEGCTL=1 xeon-e5-image
	python3 tests/boot/run-xeon-e5.py --build $(BUILD)-osxsave-neg --disk $(BUILD)/xeon-e5-disk.img --firmware bios --cpu SandyBridge-v1,model=45 --smp 16 --model 0x2d --generation sandy-bridge-ep --expect-osxsave-failure --out $(BUILD)/xeon-e5-runs/osxsave-neg

.PHONY: test-xeon-e5-size
test-xeon-e5-size: xeon-e5-image
	$(MAKE) --no-print-directory BUILD=$(BUILD)-cap-neg XEON_CPU_CAP_NEGCTL=1 xeon-e5-image
	python3 tests/unit/xeon_e5_size.py $(BUILD)/kernel.elf $(BUILD)-cap-neg/kernel.elf --out $(BUILD)/xeon-e5-runs/size.json

.PHONY: test-xeon-e5
test-xeon-e5: test-xeon-e5-cap-control test-xeon-e5-osxsave-control test-xeon-e5-size test-xeon-e5-bios test-xeon-e5-uefi
