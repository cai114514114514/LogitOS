# Ring-3 memory-protection tests (W^X, NX, SMEP/SMAP, syscall pointer validation).
#
# In its own fragment, and pulled in with a single `-include tests/sec.mk` line
# at the bottom of the Makefile, for the reason tests/audio.mk and tests/nic.mk
# give: several lines edit the Makefile at once, and a shared-file edit cannot be
# committed without swallowing theirs.
#
# The kernel side of this work needs no Makefile change at all -- C_SRC globs
# c/kernel, so c/kernel/cpu/prot.c links by existing. The only thing added to the
# build proper is /bin/secprobe, and unlike the other fragments it does NOT put
# its source in c/apps/coreutils: secprobe is a test instrument, not a coreutil,
# and it lives beside the harness that drives it in tests/boot/. That is why it
# gets an explicit rule here instead of $(call CLI_RULE,...), which hardcodes
# $(CLIDIR)/$(1).c.
#
# `CLI += secprobe` is still used: the $(DISK) recipe expands $(CLI) when it RUNS
# (deferred `=` semantics), so the .aex lands at /bin/secprobe without touching
# the shared CLI line. CLI_AEX was already expanded with :=, so the .aex is added
# as an explicit prerequisite of the disk image instead.
#
# Guarded by $(wildcard), the same way audio.mk guards sndtest.c and for the same
# reason it learned to: a concurrent commit rebuilding the tree from a stale
# index once dropped that source, and the ungarded rule then broke
# `make build/disk.img` for every line with "No rule to make target". A TEST
# fragment must not be able to break the build of the thing it tests.

.PHONY: test-sec test-sec-report

SECPROBE_SRC := $(wildcard tests/boot/secprobe.c)
ifneq ($(SECPROBE_SRC),)
CLI += secprobe

$(BUILD)/secprobe.elf: tests/boot/secprobe.c $(APPDIR)/crt0_cli.asm $(APPDIR)/clib.h $(APPDIR)/logit.h
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/secprobe.crt0c.o
	$(CC) $(UCFLAGS) -c tests/boot/secprobe.c -o $(BUILD)/apps/secprobe.cli.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/secprobe.crt0c.o $(BUILD)/apps/secprobe.cli.o

$(BUILD)/secprobe.aex: $(BUILD)/secprobe.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/secprobe.elf $@ secprobe - '*' 150 150 150

$(DISK): $(BUILD)/secprobe.aex
else
$(warning tests/sec.mk: tests/boot/secprobe.c is missing -- /bin/secprobe will not be built, and test-sec cannot run)
endif

# The gate. Boots the real machine, runs each exploit as its own ring-3 process
# from the shell, and asserts on which of them the kernel refused.
test-sec: $(ISO) $(DISK)
	@bash tests/boot/run-sec-test.sh $(ISO) $(DISK)

# The same run without the assertions: prints the verdict table. For looking at
# what the machine actually does after changing a protection, rather than for CI.
test-sec-report: $(ISO) $(DISK)
	@SEC_REPORT_ONLY=1 bash tests/boot/run-sec-test.sh $(ISO) $(DISK)
