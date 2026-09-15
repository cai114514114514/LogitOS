# Native PTY acceptance uses a private, minimal disk. The product disk currently
# contains unrelated live app work; requiring it would turn an AetherScript link
# failure into a PTY result. The serial shell, fonts, and one full-libc probe are
# everything this guest actually consumes.
PTY_DISK := $(BUILD)/pty-disk.img
PTY_OUT := $(BUILD)/pty-results
PTY_ECHO_BUILD := $(BUILD)/pty-echo-control

ifeq ($(PTY_GATE_BUILD),1)
# Measured on the first clean control build (2026-09-15): the live, out-of-scope
# AetherScript refactor added c/apps/as/runtime/io.h, and the flat -I list made
# pci.c's quoted "io.h" resolve there instead of c/kernel/cpu/io.h; the compile
# then failed on outl/inl/outw/outb. -iquote restores the kernel's intended
# header only for these isolated gate images, without editing either live tree.
CFLAGS := $(KCPU_IQ) $(CFLAGS)
endif

ifeq ($(PTY_CONTROL),echo-stuck)
# A separate BUILD receives this target-specific flag. The control leaves
# tcsetattr returning success but prevents ECHO from clearing, so only an
# observed byte on the master can satisfy the expected failure.
$(BUILD)/c/kernel/exec/fd/pty.o: CFLAGS += -DPTY_NEGCTL_ECHO_STUCK
endif

$(PTY_DISK): $(BUILD)/pty_probe.elf $(BUILD)/login.aex $(BUILD)/sh.aex $(FONTS) tools/mkfs.py tests/pty.mk
	@mkdir -p $(dir $@)
	python3 tools/mkfs.py $@ \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(BUILD)/login.aex:/bin/login $(BUILD)/sh.aex:/bin/sh \
	    $(BUILD)/pty_probe.elf:/bin/pty-check

.PHONY: pty-image test-pty test-pty-negctl test-pty-echo-negctl test-pty-pipe-negctl

pty-image:
	$(MAKE) --no-print-directory BUILD=$(BUILD) PTY_GATE_BUILD=1 $(ISO)

test-pty-echo-negctl: $(PTY_DISK)
	$(MAKE) --no-print-directory BUILD=$(PTY_ECHO_BUILD) PTY_GATE_BUILD=1 \
	    PTY_CONTROL=echo-stuck $(PTY_ECHO_BUILD)/logit.iso
	python3 tests/boot/run-pty.py --iso $(PTY_ECHO_BUILD)/logit.iso --disk $(PTY_DISK) \
	    --expect-failure 'PTY_FAIL echo disabled at line discipline' --label echo-control \
	    --out $(PTY_OUT)/echo-control.log

test-pty-pipe-negctl: pty-image $(PTY_DISK)
	python3 tests/boot/run-pty.py --iso $(ISO) --disk $(PTY_DISK) \
	    --command '/bin/pty-check pipe-control' \
	    --expect-failure 'PTY_FAIL child stdin is terminal' --label pipe-control \
	    --out $(PTY_OUT)/pipe-control.log

# Controls are prerequisites, not CI-line suggestions: neither positive guest
# run can begin unless both defects were first watched producing their named
# red assertion on a real LogitOS kernel.
test-pty-negctl: test-pty-echo-negctl test-pty-pipe-negctl

test-pty: test-pty-negctl pty-image $(PTY_DISK)
	python3 tests/boot/run-pty.py --iso $(ISO) --disk $(PTY_DISK) \
	    --label positive --out $(PTY_OUT)/positive.log

ci-boot: test-pty
