# kprof test targets -- the profiler that makes every other performance claim
# in this tree checkable.
#
# Kept in its own fragment rather than in the Makefile because several agents
# edit the Makefile at once and a shared-file edit cannot be committed without
# swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/prof.mk
#
# Nothing else is needed to BUILD the profiler: C_SRC globs c/kernel, so
# c/kernel/core/kprof.c links with no Makefile change at all.
#
#   test-prof-host     the accumulator, the spans and the report on the host,
#                      including four pthreads hammering one histogram
#   test-prof-negctl   THE SAME SUITE with -DKPROF_UNSAFE (plain `hits++`).
#                      REQUIRED TO FAIL. Without it, test-prof-host is a test
#                      that has never been seen to be capable of failing.
#   test-prof          on the machine: a workload whose 70/20/10 split is known
#                      in advance, the measured on/off overhead, and the
#                      four-core integrity check
#   test-prof-control  the on-device suite against a kernel built with
#                      -DKPROF_DISABLE. REQUIRED TO FAIL, same reason.

# The build knob. Objects are not flag-tracked (see the Makefile's note on
# CHURN/GROWFI/FPO), so toggling it means touching the sources -- which is what
# test-prof-control does.
#
#   make KPROF_OFF=1   compile the profiler out entirely: KPROF_BEGIN/END expand
#                      to nothing, the sampler never arms, /dev/kprof reports
#                      `built no`. This is both the zero-cost proof and the
#                      on-device negative control.
ifeq ($(KPROF_OFF),1)
CFLAGS += -DKPROF_DISABLE
endif

.PHONY: test-prof test-prof-host test-prof-negctl test-prof-control test-prof-all

PROF_TEST_SRC := tests/unit/prof_test.c c/kernel/core/kprof.c
PROF_TEST_INC := -Ic/kernel/core

test-prof-host:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -DLOGIT_KPROF_HOST \
	    -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(PROF_TEST_INC) -o $(BUILD)/prof_test $(PROF_TEST_SRC) -lpthread
	@$(BUILD)/prof_test

# The negative control. -DKPROF_UNSAFE swaps the CAS + atomic-add accumulator
# for the read-modify-write anybody writes first. Under four threads it loses
# tens of thousands of samples -- which is exactly the failure that would
# otherwise be invisible, because a profiler that under-counts the hottest
# address still produces a plausible-looking report.
test-prof-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -w -DLOGIT_KPROF_HOST -DKPROF_UNSAFE \
	    $(PROF_TEST_INC) -o $(BUILD)/prof_test_unsafe $(PROF_TEST_SRC) -lpthread
	@if $(BUILD)/prof_test_unsafe > $(BUILD)/prof_test_unsafe.out 2>&1; then \
	    echo "CONTROL FAILED: the non-atomic accumulator passed the four-thread test"; \
	    cat $(BUILD)/prof_test_unsafe.out; exit 1; \
	 else \
	    echo "control ok: -DKPROF_UNSAFE fails as required --"; \
	    grep -E '^  FAIL' $(BUILD)/prof_test_unsafe.out | head -6; \
	 fi

test-prof: $(ISO) $(DISK)
	@bash tests/boot/run-prof-test.sh $(ISO) $(DISK)

# The on-device negative control. Only kprof.o and kdiag.o depend on kprof.h, so
# this is a two-object rebuild and a relink, not a clean build -- and the tree is
# put back on the way out whether the control passed or not.
test-prof-control: $(DISK)
	@touch c/kernel/core/kprof.c c/kernel/core/kdiag.c
	@$(MAKE) --no-print-directory KPROF_OFF=1 $(ISO) >/dev/null
	@rc=0; bash tests/boot/run-prof-test.sh $(ISO) $(DISK) \
	    > $(BUILD)/prof_control.out 2>&1 || rc=$$?; \
	 touch c/kernel/core/kprof.c c/kernel/core/kdiag.c; \
	 $(MAKE) --no-print-directory $(ISO) >/dev/null; \
	 if [ "$$rc" = 0 ]; then \
	    echo "CONTROL FAILED: the harness passed against a kernel with no profiler in it"; \
	    tail -30 $(BUILD)/prof_control.out; exit 1; \
	 else \
	    echo "control ok: -DKPROF_DISABLE fails the on-device suite as required --"; \
	    grep -E '^  FAIL' $(BUILD)/prof_control.out | head -8; \
	 fi

test-prof-all: test-prof-host test-prof-negctl test-prof
	@echo "test-prof-all: ALL PASS"
