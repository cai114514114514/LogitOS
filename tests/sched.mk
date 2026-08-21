# Weighted scheduling: /bin/nice, /bin/renice, /bin/schedtest, and the gate.
#
# Its own fragment, pulled in by tests/thread.mk's last line, for the reason
# tests/thread.mk and tests/audio.mk both give: several lines are editing the
# Makefile at once and a shared-file edit cannot be committed without
# swallowing theirs. The kernel side needs no build-system change at all --
# C_SRC globs c/kernel, so the changes in c/kernel/sched/sched.c link by
# existing.
#
# WHY IT HANGS OFF thread.mk RATHER THAN ITS OWN -include LINE: adding
# `-include tests/sched.mk` to the Makefile is a one-line edit to the one file
# this line was told not to touch. One line in an already-included fragment
# costs the same and conflicts with nobody.

SCHED_SRC := $(wildcard $(CLIDIR)/schedtest.c)

# nice/renice are ordinary clib.h CLI programs, so CLI_RULE builds them. They
# are guarded the same way schedtest is below: a test fragment must not be able
# to break the build of the thing it tests (tests/audio.mk's note -- a
# concurrent commit once rebuilt from a stale index and dropped a test source,
# and the ungated rule made `make build/disk.img` fail for every line).
ifneq ($(wildcard $(CLIDIR)/nice.c),)
$(eval $(call CLI_RULE,nice))
CLI += nice
$(DISK): $(BUILD)/nice.aex
endif
ifneq ($(wildcard $(CLIDIR)/renice.c),)
$(eval $(call CLI_RULE,renice))
CLI += renice
$(DISK): $(BUILD)/renice.aex
endif

ifneq ($(SCHED_SRC),)
# schedtest CANNOT use CLI_RULE, for the same reason thrtest cannot: it is
# built against MINI-LIBC, not clib.h's inline syscalls, because half of what
# it exists to check is the POSIX surface -- getpriority/setpriority/nice and
# their errno behaviour. A clib.h build would test the raw syscall and leave
# c/apps/libc/src/resource.c's new code with no caller anywhere in the tree.
#
# It does NOT link c/apps/libc/logit_tls.ld: nothing here uses __thread, and
# thrtest's header explains why that linker script is deliberately opt-in.
#
# $(1) = program name, $(2) = extra -D flags.
define SCHEDTEST_RULE
$(BUILD)/schedobj/$(1).o: $(CLIDIR)/schedtest.c c/apps/libc/include/sys/resource.h
	@mkdir -p $$(dir $$@)
	$(CC) $(UCFLAGS) $(2) -c $$< -o $$@
$(BUILD)/$(1).elf: $(BUILD)/schedobj/$(1).o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $$@ \
	    $(BUILD)/apps/$(1).crt0c.o $(BUILD)/schedobj/$(1).o $(LIBC_OBJS)
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
endef

$(eval $(call SCHEDTEST_RULE,schedtest,))
CLI += schedtest
$(DISK): $(BUILD)/schedtest.aex
else
$(warning tests/sched.mk: $(CLIDIR)/schedtest.c is missing -- /bin/schedtest will not be built, and test-sched cannot run)
endif

.PHONY: test-sched test-sched-negctl

# The positive gate: three ratio cases (1:1, 2:1, 4:1) plus the API checks, on
# a real boot. Two cores, because one core makes "who runs next" the only
# question and two makes the ring's ordering matter as well -- and because the
# machine this ships on is not single-core.
test-sched: $(ISO) $(DISK)
	@sh tests/boot/run-sched-test.sh $(ISO) $(DISK)

# THE NEGATIVE CONTROL, and it needs a differently-built KERNEL rather than a
# differently-built program -- which is why it goes through a wrapper makefile
# instead of a -D on one recipe. tests/schedneg.mk is `include Makefile` plus
# one flag; BUILD is overridden so the instrumented objects land in their own
# tree and cannot be mistaken for the shipped ones (the Makefile's own debug-knob
# note says objects are not flag-tracked, so sharing build/ would silently
# poison the next ordinary build).
#
# -DSCHED_IGNORE_WEIGHT is placed at the single line in prio_apply() that turns
# a weight into the reciprocal the charge uses. Everything else still works:
# nice is stored, getpriority reads it back, SCHEDCTL_GET_WEIGHT still reports
# 512 for nice 10. So the control is the PLAUSIBLE wrong implementation -- "I
# wired the syscall and the table and never made the pick loop use them" -- and
# the run.sh below requires the 2:1 and 4:1 cases to redden while the 1:1 case
# and all four API checks keep passing. A control that reddens everything
# proves only that the build changed.
test-sched-negctl: $(DISK)
	@$(MAKE) -f tests/schedneg.mk BUILD=build/schedneg build/schedneg/logit.iso
	@sh tests/boot/run-sched-negctl.sh build/schedneg/logit.iso $(DISK)

# Named on the positive target so it cannot become one of the STRANDED CONTROLS
# CLAUDE.md counts -- a separate *-negctl target that no suite reaches is run
# never while looking exactly like a control that is covered. Confirmed against
# the audit rather than assumed: `python3 tools/audit_tests.py` lists
# test-sched-negctl nowhere under STRANDED, and did list it before this line.
test-sched: test-sched-negctl

# And the positive itself is named on a suite, or the whole thing is debt.
# `python3 tools/audit_tests.py` reported test-sched under "UNWIRED (NEW)" until
# this line, which is the state CLAUDE.md describes as looking exactly like
# coverage while being none. ci-boot rather than ci-host because it boots QEMU;
# the cost it adds is two boots of about 30 s each plus, the first time only, a
# second kernel tree for the control (incremental after that).
ci-boot: test-sched
