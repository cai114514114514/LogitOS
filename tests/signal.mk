# Signal test targets.
#
# In its own fragment for the reason tests/audio.mk and tests/kbench.mk give:
# a dozen lines are editing the Makefile at once and a shared-file edit cannot
# be committed without swallowing theirs. The Makefile pulls this in with one
# line:  -include tests/signal.mk
#
# The kernel side needs no Makefile change at all -- C_SRC globs c/kernel, so
# c/kernel/exec/ksignal.c and ksigframe.c link by existing. What this adds is
# /bin/sigtest, a disk to put it on, and the two targets.

.PHONY: test-signal test-signal-negctl test-signal-all

# --- the -DSIGNAL_NO_FPU build ----------------------------------------------
# THE NEGATIVE CONTROL, and the reason it is this and not "remove signals".
#
# A kernel that delivers signals correctly and does NOT save the FPU/SSE state
# in the frame passes every test anyone would think to write: handlers run,
# kill -TERM works, SIGCHLD arrives, the shell is fine. What breaks is
# ARITHMETIC -- silently, and only when a signal lands between a value being
# computed and being used, because c/boot/isr.asm FXSAVEs on every kernel entry
# precisely because the kernel is built with -msse2 and a handler is ring-3 C
# compiled the same way.
#
# So this flag is the sabotage, it is one #ifdef in ksigframe.c, and
# test-signal-negctl REQUIRES the suite to fail with it. Same shape as
# tests/kbench.mk's KBENCH_NEGCTL: a guarded CFLAGS line, a touch of the
# affected sources, a rebuild, a run, and a rebuild back.
#
# CFLAGS is appended to here rather than in the Makefile proper; the -include
# is late, but CFLAGS in a recipe is expanded when the recipe RUNS, so a late
# append still reaches every kernel object.
ifeq ($(SIGNAL_NO_FPU),1)
CFLAGS += -DSIGNAL_NO_FPU
endif

# --- /bin/sigtest -------------------------------------------------------------
# Links the real mini-libc, exactly as build/libctest.aex does, because the
# thing under test includes mini-libc's sigaction()/restorer and a test of a
# reimplementation would prove nothing about the shipped one.
#
# Guarded by $(wildcard) for the reason tests/audio.mk states and has already
# had to: a test fragment must not be able to break the build of the thing it
# tests, so a missing source means no /bin/sigtest and a warning, not a
# "No rule to make target" for every other line in the tree.
SIGTEST_SRC := $(wildcard tests/unit/sigtest_main.c)
ifneq ($(SIGTEST_SRC),)

$(BUILD)/asobj/tests/unit/sigtest_main.o: tests/unit/sigtest_main.c
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/sigtest.elf: $(BUILD)/asobj/tests/unit/sigtest_main.o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/sigtest.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/sigtest.crt0c.o $(BUILD)/asobj/tests/unit/sigtest_main.o $(LIBC_OBJS)

$(BUILD)/sigtest.aex: $(BUILD)/sigtest.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/sigtest.elf $@ sigtest - '?' 150 150 150

# --- a disk of its own, and why ----------------------------------------------
# $(DISK) carries every app on the machine, including browser.aex, which drags
# in QuickJS and LibCSS -- three hundred and some translation units, minutes of
# build, for a test whose entire vocabulary is /bin/sh and /bin/sigtest. Two
# reasons to build a small one instead, and the second is not about speed:
#
#   1. This test boots in seconds and can be run after every edit to the signal
#      path, which is the only way a delivery bug gets found while the change
#      that caused it is still in front of you.
#   2. IT DOES NOT SHARE A FAILURE MODE WITH THE REST OF THE TREE. As of this
#      writing `make build/disk.img` does not build at all --
#      c/apps/browser/js_dom.c writes __attribute__((weak)) and the JS build
#      passes -include features.h, whose `#define weak __attribute__((__weak__))`
#      expands inside it. That is somebody else's line to fix, and a signal test
#      that cannot run until they do is a signal test nobody runs.
#
# /bin/sh is not optional: wm_run proc_spawns it as the console, so a disk
# without it boots to a machine with nothing to type at. The fonts are here
# because text_init() looks for them after mount and a missing font is noise in
# the log that a reader then has to dismiss.
SIGDISK := $(BUILD)/sigdisk.img
$(SIGDISK): $(BUILD)/sigtest.aex $(BUILD)/sh.aex $(BUILD)/echo.aex $(FONTS) tools/mkfs.py
	@mkdir -p $(BUILD)
	python3 tools/mkfs.py $(SIGDISK) \
	    fsroot/fonts/ui.ttf:/fonts/ui.ttf fsroot/fonts/mono.ttf:/fonts/mono.ttf \
	    $(BUILD)/sh.aex:/bin/sh $(BUILD)/echo.aex:/bin/echo \
	    $(BUILD)/sigtest.aex:/bin/sigtest

# The real test: boot, run /bin/sigtest on the serial console, require
# SIGTEST_OK. Everything it checks is in the header comment of
# tests/unit/sigtest_main.c.
test-signal: $(ISO) $(SIGDISK)
	@bash tests/boot/run-signal-test.sh $(ISO) $(SIGDISK)

# REQUIRED TO FAIL. Rebuilds the kernel with the FPU state left out of the
# signal frame and runs the identical suite; if it still passes, the FPU checks
# in that suite are not checking anything and this line's central claim is
# unsupported. Rebuilds the ordinary kernel afterwards either way, so a failed
# run does not leave a sabotaged ISO behind for the next target to boot.
SIGNAL_NEGCTL_SRC := c/kernel/exec/ksigframe.c
test-signal-negctl: $(SIGDISK)
	@touch $(SIGNAL_NEGCTL_SRC)
	@$(MAKE) --no-print-directory SIGNAL_NO_FPU=1 $(ISO) >/dev/null
	@bash tests/boot/run-signal-test.sh $(ISO) $(SIGDISK) >$(BUILD)/signal_negctl.txt 2>&1; \
	 rc=$$?; \
	 touch $(SIGNAL_NEGCTL_SRC); \
	 $(MAKE) --no-print-directory $(ISO) >/dev/null; \
	 if [ $$rc -eq 0 ]; then \
	    echo "FAIL: with the FPU/SSE state left OUT of the signal frame the suite"; \
	    echo "      still PASSED -- so nothing in it actually checks that a handler"; \
	    echo "      preserves the interrupted computation's XMM registers."; \
	    exit 1; \
	 else \
	    echo "PASS (negative control): -DSIGNAL_NO_FPU fails the suite, as required --"; \
	    grep -aE "XMM|FAIL:" $(BUILD)/signal_negctl.txt | head -12 | sed 's/^/       /'; \
	 fi

# The aggregate. Both halves, in the order that makes a failure readable: the
# real one first, so a broken build is reported as a broken build rather than
# as a negative control that "correctly failed".
test-signal-all:
	@$(MAKE) --no-print-directory test-signal
	@$(MAKE) --no-print-directory test-signal-negctl

else
$(warning tests/signal.mk: tests/unit/sigtest_main.c is missing -- /bin/sigtest will not be built and the signal targets cannot run)
endif
