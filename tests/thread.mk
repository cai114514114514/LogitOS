# M30 threads: /bin/thrtest and its four negative controls.
#
# Its own fragment rather than lines in the Makefile for the reason tests/audio.mk
# and tests/nic.mk give: several lines are editing the Makefile at once, and a
# shared-file edit cannot be committed without swallowing theirs. The kernel side
# needs no Makefile change at all -- C_SRC globs c/kernel, so c/kernel/sched/
# uthread.c links by existing -- and mini-libc's pthread.c and pthread_entry.asm
# are picked up by the existing wildcards over c/apps/libc/src.
#
# thrtest cannot use CLI_RULE. Those programs are built against clib.h's inline
# syscalls; this one is built against mini-libc, because THE POINT IS TO TEST THE
# PTHREADS SURFACE rather than the raw syscalls under it. So it links
# $(LIBC_OBJS) at the common CLI base, exactly as /bin/as and /bin/libctest do.
#
# IT ALSO LINKS -T c/apps/libc/logit_tls.ld, and that is the load-bearing part:
# it is what makes `__thread` work (see the header of that file for why the
# PT_TLS bounds cannot be discovered any other way here). thrtest is currently
# the ONLY program in the tree that links it, which is why thrtest is where
# "__thread is per-thread" is proved and why adding it to the browser, /bin/as
# and the JS app is a separate, deliberate step.

THRTEST_SRC := $(wildcard $(CLIDIR)/thrtest.c)
ifneq ($(THRTEST_SRC),)

# $(1) = program name, $(2) = extra -D flags.
# Guarded by the wildcard above for the reason tests/audio.mk documents: a
# concurrent commit once rebuilt from a stale index and dropped a test's source,
# and the rule that assumed it was there made `make build/disk.img` fail for
# every line in the tree. A test fragment must not be able to break the build of
# the thing it tests.
define THRTEST_RULE
$(BUILD)/throbj/$(1).o: $(CLIDIR)/thrtest.c c/apps/libc/include/pthread.h
	@mkdir -p $$(dir $$@)
	$(CC) $(UCFLAGS) $(2) -c $$< -o $$@
$(BUILD)/$(1).elf: $(BUILD)/throbj/$(1).o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm c/apps/libc/logit_tls.ld
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -T c/apps/libc/logit_tls.ld -o $$@ \
	    $(BUILD)/apps/$(1).crt0c.o $(BUILD)/throbj/$(1).o $(LIBC_OBJS)
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
endef

$(eval $(call THRTEST_RULE,thrtest,))

# `CLI +=` is deferred and the $(DISK) recipe expands $(CLI) when it RUNS, so
# this lands the program under /bin without touching the shared CLI list.
# CLI_AEX was already expanded with :=, so the .aex is added as an explicit
# prerequisite of the disk image instead. (Same mechanism as /bin/sndtest;
# CLI_RULE is deliberately NOT invoked -- the rule above replaces it.)
CLI += thrtest
$(DISK): $(BUILD)/thrtest.aex

# THE NEGATIVE CONTROLS, on their own disk image.
#
# Four builds of the same program with one guard removed from each, and every
# one of them MUST fail. They are packed only when THR_NEGCTL is set, so a
# shipped disk can never contain a deliberately broken program -- the same shape
# test-monitor-negctl uses for its crippled Monitor.
#
#   thrtest-serial   creates and joins one at a time -> the wall-clock speedup
#                    assertion fails. This is the control that gives the PASSING
#                    run its meaning: without it, "T4 < 2*T1" is a number nobody
#                    has seen come out the other way.
#   thrtest-tls      reads the per-thread value out of a shared global -> the
#                    "each thread read back its own value" assertion fails.
#   thrtest-nolock   drops the mutex around a split read-modify-write -> the
#                    counter comes out short.
#   thrtest-leak     never detaches -> descriptors are never returned and
#                    pthread_create starts failing partway through.
ifdef THR_NEGCTL
$(eval $(call THRTEST_RULE,thrtest-serial,-DTHR_NEGCTL_SERIAL))
$(eval $(call THRTEST_RULE,thrtest-tls,-DTHR_NEGCTL_TLS))
$(eval $(call THRTEST_RULE,thrtest-nolock,-DTHR_NEGCTL_NOLOCK))
$(eval $(call THRTEST_RULE,thrtest-leak,-DTHR_NEGCTL_LEAK))
CLI += thrtest-serial thrtest-tls thrtest-nolock thrtest-leak
$(DISK): $(BUILD)/thrtest-serial.aex $(BUILD)/thrtest-tls.aex \
         $(BUILD)/thrtest-nolock.aex $(BUILD)/thrtest-leak.aex
endif

else
$(warning tests/thread.mk: $(CLIDIR)/thrtest.c is missing -- /bin/thrtest will not be built, and test-thread cannot run)
endif

.PHONY: test-thread test-thread-negctl

test-thread: $(ISO) $(DISK)
	@sh tests/boot/run-thread-test.sh $(ISO) $(DISK)

test-thread-negctl:
	@$(MAKE) THR_NEGCTL=1 DISK=$(BUILD)/disk_thrneg.img $(BUILD)/disk_thrneg.img
	@sh tests/boot/run-thread-negctl.sh $(ISO) $(BUILD)/disk_thrneg.img
