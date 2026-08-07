# kbench -- what the kernel's own primitives cost.
#
# In its own fragment rather than in the Makefile because a dozen agents edit
# the Makefile at once and a shared-file edit cannot be committed without
# swallowing theirs. The Makefile pulls it in with one line:
#
#     -include tests/kbench.mk
#
# Nothing else is needed to BUILD it: C_SRC globs c/kernel, so
# c/kernel/sched/kbench.c links with no Makefile change at all.
#
#   test-kbench          boot the machine, run a fork/exec workload through it,
#                        and assert on the numbers the kernel measured. Three
#                        of the assertions are chosen to FAIL against the
#                        kernel as it was before this work -- see the header of
#                        tests/boot/run-kbench.sh for the before/after values.
#   test-kbench-5        the same, five boots, median across them. The host is
#                        shared with other agents' QEMU, so one boot is a
#                        sample and not a measurement.
#   test-kbench-1core    the same on -smp 1. The interesting contrast is the
#                        BKL block of the report: with one core, `contended`
#                        must be 0 and `wait` must be ~0. If it is not, the
#                        accounting is measuring itself.

#   test-kbench-negctl   THE SAME HARNESS against a kernel built with
#                        -DKBENCH_NEGCTL, which puts back the three things this
#                        line changed: SYS_GET_TIME reads the CMOS on every
#                        call, execve maps the whole megabyte of user stack up
#                        front, and waitpid polls instead of parking. REQUIRED
#                        TO FAIL. Without it, test-kbench is a test that has
#                        never been seen to be capable of failing -- and one of
#                        its assertions (anonymous faults >= 1) is a correctness
#                        gate on the demand-paged stack, so "it passes" has to
#                        mean something.
#   test-kbench-micro    the primitive-cost table (this_cpu, spinlock, rdtsc,
#                        the ns clock, fxsave, frame allocation at each poison
#                        level, context switch, kprintf). It needs a kernel
#                        built with KBENCH=1, because that battery is CPU-bound
#                        kernel code holding the big kernel lock and stalls the
#                        desktop for a measured 270 ms -- see the note at its
#                        call site. Not something to inject into every boot of a
#                        tree twenty other lines run harnesses against.
ifeq ($(KBENCH),1)
CFLAGS += -DKBENCH_MICRO
endif

ifeq ($(KBENCH_NEGCTL),1)
CFLAGS += -DKBENCH_NEGCTL
endif

.PHONY: test-kbench test-kbench-5 test-kbench-1core test-kbench-negctl test-kbench-micro

test-kbench: $(ISO) $(DISK)
	@bash tests/boot/run-kbench.sh $(ISO) $(DISK) 1

test-kbench-5: $(ISO) $(DISK)
	@bash tests/boot/run-kbench.sh $(ISO) $(DISK) 5

test-kbench-1core: $(ISO) $(DISK)
	@KBENCH_SMP=1 bash tests/boot/run-kbench.sh $(ISO) $(DISK) 1

# Objects are not flag-tracked (same note as CHURN/GROWFI/KPROF_OFF), so the
# three affected translation units are touched on the way in and on the way out,
# and the tree is rebuilt back to normal whether the control passed or not.
# Only kbench.o depends on KBENCH_MICRO, so this is a one-object rebuild and a
# relink each way, and the tree is put back whether the run passed or not.
test-kbench-micro: $(DISK)
	@touch c/kernel/sched/kbench.c
	@$(MAKE) --no-print-directory KBENCH=1 $(ISO) >/dev/null
	@rc=0; bash tests/boot/run-kbench.sh $(ISO) $(DISK) 1 || rc=$$?; \
	 touch c/kernel/sched/kbench.c; \
	 $(MAKE) --no-print-directory $(ISO) >/dev/null; \
	 exit $$rc

KBENCH_NEGCTL_SRC := c/kernel/exec/syscall.c c/kernel/exec/exec.c c/kernel/exec/proc.c

test-kbench-negctl: $(DISK)
	@touch $(KBENCH_NEGCTL_SRC)
	@$(MAKE) --no-print-directory KBENCH_NEGCTL=1 $(ISO) >/dev/null
	@rc=0; bash tests/boot/run-kbench.sh $(ISO) $(DISK) 1 \
	    > $(BUILD)/kbench_negctl.out 2>&1 || rc=$$?; \
	 touch $(KBENCH_NEGCTL_SRC); \
	 $(MAKE) --no-print-directory $(ISO) >/dev/null; \
	 if [ "$$rc" = 0 ]; then \
	    echo "CONTROL FAILED: the harness passed against a kernel with the old behaviour in it"; \
	    tail -30 $(BUILD)/kbench_negctl.out; exit 1; \
	 else \
	    echo "control ok: -DKBENCH_NEGCTL fails the harness as required --"; \
	    grep -E '^FAIL:' $(BUILD)/kbench_negctl.out | head -8; \
	    grep -E '^(SYS_GET_TIME|pages shared|bkl_hlt_wait|anonymous)' \
	         $(BUILD)/kbench_negctl.out | head -8; \
	 fi
