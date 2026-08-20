# poll(), eventfd and timerfd: the host gate, its negative control, and the
# on-device half.
#
# INCLUDED FROM tests/exec.mk, not from the Makefile, and that is deliberate
# rather than lazy. The top-level Makefile is being edited by several lines of
# work at once and a shared-file edit cannot be committed without swallowing
# theirs; `-include` nests, so hanging this off the fragment that already covers
# c/kernel/exec costs nobody a merge. IF SOMEONE LATER WANTS IT DIRECT, the one
# line is `-include tests/poll.mk` beside the others near the end of the
# Makefile, and this include at the bottom of tests/exec.mk comes out.
#
# The kernel side needs no build-system change at all: C_SRC globs c/kernel, so
# c/kernel/exec/kpoll.c and kpollsys.c link by existing, and mini-libc's
# poll.c is picked up by the existing wildcard over c/apps/libc/src.

.PHONY: test-poll test-poll-negctl test-poll-os

# ---------------------------------------------------------------------------
# THE HOST GATE.
#
# It compiles the REAL c/kernel/exec/kpoll.c and the REAL c/kernel/core/wait.c
# against a modelled scheduler (tests/unit/pollhost/hostsched.c). Read that
# file's header for exactly what is real and what is modelled -- the short
# version is that the wait queues, the poll hook and the ticket spinlock are the
# tree's own code, and only park/unpark is a pthread condvar.
#
# -iquote AND NOT -I, and this is load-bearing. The kernel headers this needs
# include sched.h and wait.h, whose basenames collide with the host's own
# <sched.h> and with mini-libc's <sys/wait.h>. With -I, glibc's <pthread.h>
# does `#include <sched.h>` and gets the KERNEL's scheduler header -- the build
# then fails on `unknown type name cpu_set_t` inside /usr/include/pthread.h,
# which points at nothing. -iquote applies only to "quoted" includes, so the
# kernel's own `#include "sched.h"` resolves and the system's does not. Same
# flat-namespace trap CLAUDE.md records twice, met from the host side.
POLL_HOST_SRC := tests/unit/poll_test.c tests/unit/pollhost/hostsched.c \
                 c/kernel/core/wait.c c/kernel/exec/kpoll.c
POLL_HOST_INC := -iquote c -iquote c/kernel/core -iquote c/kernel/cpu \
                 -iquote c/kernel/sched -iquote c/kernel/exec \
                 -iquote c/drivers/timer -iquote include/abi
# ASan + UBSan: this code is a linked list mutated from several threads under a
# hand-written lock, which is the shape where a use-after-free is silent.
POLL_HOST_CF  := -O1 -g -Wall -Wextra -fsanitize=address,undefined -pthread

$(BUILD)/poll_test: $(POLL_HOST_SRC) c/kernel/exec/kpoll.h c/kernel/core/wait.h
	@mkdir -p $(BUILD)
	$(CC) $(POLL_HOST_CF) -o $@ $(POLL_HOST_SRC) $(POLL_HOST_INC)

$(BUILD)/poll_test_negctl: $(POLL_HOST_SRC) c/kernel/exec/kpoll.h c/kernel/core/wait.h
	@mkdir -p $(BUILD)
	$(CC) $(POLL_HOST_CF) -DPOLL_NO_PREREGISTER -o $@ $(POLL_HOST_SRC) $(POLL_HOST_INC)

test-poll: $(BUILD)/poll_test
	@$(BUILD)/poll_test

# THE NEGATIVE CONTROL, and what it changes is one thing only: poll_core()
# reads every source's readiness BEFORE registering on its wait queue instead
# of after. Everything else is identical, every ordinary case still passes, and
# the ONLY checks that redden are the lost-wakeup ones -- which is the point.
# A control that broke twenty checks would prove that -D did something, not
# that the registration order is what closes the race.
test-poll-negctl: $(BUILD)/poll_test_negctl
	@echo "--- poll negative control: readiness read BEFORE registration ---"
	@if $(BUILD)/poll_test_negctl; then \
	    echo "NEGCTL FAILED TO FAIL: the lost wakeup did not reproduce"; exit 1; \
	 else \
	    echo "negctl reddened as required (see the two LOST WAKEUP lines above)"; \
	 fi

# Named as a prerequisite of the positive gate rather than beside it on a
# ci- line: CLAUDE.md's STRANDED CONTROLS category exists because a control
# that is a separate target and is named by nobody runs never while looking
# exactly like one that is covered.
test-poll: test-poll-negctl

# ---------------------------------------------------------------------------
# THE ON-DEVICE HALF: /bin/polltest.
#
# It cannot use CLI_RULE, for the reason tests/thread.mk gives about thrtest:
# those programs are built against clib.h's inline syscalls, and the point here
# is to test the POSIX surface -- poll(), select(), eventfd(), timerfd() and
# pthreads -- so it links $(LIBC_OBJS) at the common CLI base like /bin/as and
# /bin/thrtest do. It also needs logit_tls.ld, because it creates threads.
POLLTEST_SRC := $(wildcard $(CLIDIR)/polltest.c)
ifneq ($(POLLTEST_SRC),)

# Guarded by the wildcard above for the reason tests/audio.mk documents: a
# concurrent commit once rebuilt from a stale index and dropped a test's source,
# and the rule that assumed it was there broke `make build/disk.img` for every
# line in the tree. A test fragment must not be able to break the build of the
# thing it tests.
define POLLTEST_RULE
$(BUILD)/pollobj/$(1).o: $(CLIDIR)/polltest.c c/apps/libc/include/poll.h \
                         c/apps/libc/include/sys/eventfd.h \
                         c/apps/libc/include/sys/timerfd.h
	@mkdir -p $$(dir $$@)
	$(CC) $(UCFLAGS) $(2) -c $$< -o $$@
$(BUILD)/$(1).elf: $(BUILD)/pollobj/$(1).o $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm c/apps/libc/logit_tls.ld
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/$(1).crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -T c/apps/libc/logit_tls.ld -o $$@ \
	    $(BUILD)/apps/$(1).crt0c.o $(BUILD)/pollobj/$(1).o $(LIBC_OBJS)
$(BUILD)/$(1).aex: $(BUILD)/$(1).elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/$(1).elf $$@ $(1) - '*' 150 150 150
endef

$(eval $(call POLLTEST_RULE,polltest,))

# `CLI +=` is deferred and the $(DISK) recipe expands $(CLI) when it RUNS, so
# this lands the program under /bin without touching the shared CLI list.
CLI += polltest
$(DISK): $(BUILD)/polltest.aex

# THE DEVICE NEGATIVE CONTROL, on its own disk image: every poll() replaced by
# a sleep that reports nothing ready, which is EXACTLY what mini-libc's poll()
# did before SYS_POLL existed. Packed only when POLL_NEGCTL is set, so a shipped
# disk never carries a deliberately broken program.
ifdef POLL_NEGCTL
$(eval $(call POLLTEST_RULE,polltest-nopoll,-DPOLLTEST_NEGCTL_NOPOLL))
CLI += polltest-nopoll
$(DISK): $(BUILD)/polltest-nopoll.aex
endif

else
$(warning tests/poll.mk: $(CLIDIR)/polltest.c is missing -- /bin/polltest will not be built, and test-poll-os cannot run)
endif

test-poll-os: $(ISO) $(DISK)
	@bash tests/boot/run-poll-test.sh $(ISO) $(DISK)

.PHONY: test-poll-os-negctl
test-poll-os-negctl:
	@$(MAKE) POLL_NEGCTL=1 DISK=$(BUILD)/disk_pollneg.img $(BUILD)/disk_pollneg.img
	@bash tests/boot/run-poll-negctl.sh $(ISO) $(BUILD)/disk_pollneg.img

# Same reasoning as above: a control nobody names runs never.
test-poll-os: test-poll-os-negctl
