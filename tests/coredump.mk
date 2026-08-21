# Core dumps -- c/kernel/exec/coredump.{c,h}, c/apps/coreutils/corefmt.h.
#
# INCLUDED FROM tests/exec.mk, not from the Makefile, for the reason
# tests/poll.mk states at its own head and which has not changed: the top-level
# Makefile is edited by several lines of work at once, `-include` nests, and
# hanging this off the fragment that already covers c/kernel/exec costs nobody a
# merge. If someone later wants it direct, the one line is
# `-include tests/coredump.mk` beside the others near the end of the Makefile,
# and the include at the bottom of tests/exec.mk comes out.
#
# THE KERNEL SIDE NEEDS NO BUILD-SYSTEM CHANGE. C_SRC is
# `find c/kernel c/drivers c/lib c/fs c/net c/crypto -name '*.c'`
# (Makefile:253), so c/kernel/exec/coredump.c links by existing, and
# fsroot/as/examples/*.as is a wildcard (Makefile:25) so the on-device fixture
# reaches /usr/as/examples/ the same way.
#
# WHAT DOES NEED A LINE, stated here rather than left to be discovered:
# /bin/readcore and /bin/crash are not on the disk. Makefile:460's CLI list is
# hand-written and this work does not own that file. Adding `readcore crash` to
# it puts both on the image with no other change -- CLI_RULE and the $(DISK)
# recipe's `$(foreach c,$(CLI),...)` do the rest. Until then:
#   * the READER is still gated, because tests/unit/corecheck.c includes the
#     identical parser (c/apps/coreutils/corefmt.h) and both gates run it;
#   * the SENTINEL REGISTERS crash.c would put in r12..r15 are not measured on
#     device, only host-side. That is the one thing this omission costs, and it
#     is why test-coredump-os compares rip/rsp between the two channels the
#     kernel itself prints instead.

.PHONY: test-coredump test-coredump-negctl test-coredump-os

CORE_SRC  := tests/unit/coredump_test.c c/kernel/exec/coredump.c
CORE_INC  := -iquote c/kernel/exec -iquote c/kernel/cpu -iquote c/apps/coreutils
# -iquote AND NOT -I, the same trap tests/poll.mk documents: this test includes
# <sys/procfs.h> and <sys/user.h>, and c/kernel's flat header namespace holds
# basenames glibc also uses. With -I the system headers start resolving into
# c/kernel and the failure surfaces inside /usr/include, pointing at nothing.
#
# -DLOGIT_COREDUMP_HOST compiles ONLY the builder half of coredump.c. The kernel
# half below that guard calls vfs_write and kprintf and could not link here; the
# builder is compiled from the same file, unmodified, byte for byte the code the
# kernel runs.
CORE_CF   := -O1 -g -Wall -Wextra -Wno-unused-parameter -DLOGIT_COREDUMP_HOST

$(BUILD)/coredump_test: $(CORE_SRC) c/kernel/exec/coredump.h c/apps/coreutils/corefmt.h
	@mkdir -p $(BUILD)
	$(CC) $(CORE_CF) -o $@ $(CORE_SRC) $(CORE_INC)

$(BUILD)/corecheck: tests/unit/corecheck.c c/kernel/exec/coredump.h c/apps/coreutils/corefmt.h
	@mkdir -p $(BUILD)
	$(CC) -O1 -g -Wall -Wextra -o $@ tests/unit/corecheck.c $(CORE_INC)

# --- test-coredump ---------------------------------------------------------
# 128 checks, in six parts:
#   1  the NT_PRSTATUS / NT_PRPSINFO layouts and the 27-register order, diffed
#      field by field against glibc's <sys/procfs.h> and <sys/user.h>. This is
#      what decides whether "ELF core" is a claim or a costume.
#   2  a build over a modelled address space with a hole in it
#   3  our SECOND reader (c/apps/coreutils/corefmt.h, /bin/readcore's parser)
#   4  gdb and readelf, neither of which came from this tree: gdb has to report
#      the same rip, rsp, r15, r12 and rbx, the same signal, the same program
#      name, and the same bytes at the same addresses
#   5  the size cap -- a truncated dump must still be a well-formed ELF core,
#      must still carry the register file, must set CORE_F_TRUNCATED, and must
#      not have a single PT_LOAD claiming bytes that are not in the file
#   6  under the cap, the STACK survives. A deliberately upside-down address
#      map (big heap low, stack high) so that ascending-address order would
#      spend the whole cap before reaching the stack. Constructed because on
#      the real machine the two are the other way round -- the property would
#      otherwise hold by luck of the memory map and break the first time a
#      program was linked differently.
#
# The control runs WITH it, as a prerequisite rather than a recipe line, for the
# reason tests/exec.mk gives: tools/audit_tests.py excludes `test-*-negctl` from
# suite discovery on the ground that the positive counterpart runs it, and a
# recipe line is the half of that arrangement which can be lost to an edit.
test-coredump: test-coredump-negctl
test-coredump: $(BUILD)/coredump_test
	@$(BUILD)/coredump_test

# --- test-coredump-negctl --------------------------------------------------
# -DCOREDUMP_SIGCTX builds the dump from the DUMPER's context instead of the
# trap frame -- the mistake a real implementation makes the moment it moves the
# dump out of the trap handler and into a signal-delivery path or proc_exit(),
# where the trap frame is no longer in scope and "the registers" are whatever
# is live. The control is deliberately the PLAUSIBLE wrong implementation and
# not the absent one: it reads the callee-saved registers off the CPU, takes a
# real return address for rip and a real frame address for rsp, and copies cs,
# ss and rflags from the true frame.
#
# THE PROPERTY ASSERTED IS THE SET, NOT THE COUNT, AND THE REASON IS WORTH MORE
# THAN THE NUMBER WOULD BE.
#
# 19 checks carry the REGFILE prefix and are the only ones that may redden. The
# number that actually reddens is between 8 and 19 and MOVES WITH THE COMPILER,
# measured on this machine on 2026-08-20:
#
#     gcc 15.2.0 -O1 -w    17 of 128   (2026-08-20)
#     an earlier build of the same source          15 of 121
#
# The two that survived were `rbx` and `gdb: rbx`, and the explanation is not a
# flaw in the -D. The control reads the CALLEE-SAVED registers off the CPU,
# because that is what a dump taken from a signal-handling context would get.
# But the code that built the correct frame has just been moving those very
# values around, so a callee-saved register can legitimately still be holding
# the value it is supposed to be reporting -- rbx held 0xC0DE...B8 because the
# compiler had put r->rbx there. That is a real and permanent limitation of
# this shape of control over r12..r15/rbx/rbp, and pinning "== 17" would turn it
# into a test that fails on a different optimisation level for a reason that has
# nothing to do with core dumps -- it moved from 15 to 17 in this session on an
# edit that touched neither the control nor those checks.
#
# So three things are asserted instead, and together they are stronger than a
# count:
#   1. EVERY reddened check is a REGFILE check. Nothing outside the register
#      file may break -- this is the real statement, and it is what stops a -D
#      that merely damages the build from counting as a control.
#   2. The EIGHT checks that cannot coincide by construction all redden: rip and
#      rsp (a return address and a frame address can never be 0x40001234 /
#      0x40001f80), rax and rdi (caller-saved, so the control leaves them zero),
#      coredump_read_gregs, gdb's own rip and rsp, and the register check on the
#      TRUNCATED build.
#   3. Four named non-register checks stay GREEN.
#
# TWO OF THE NINETEEN ARE NOT ABOUT REGISTERS AT ALL, AND THAT IS THE MOST
# USEFUL THING THE CONTROL SAID. Part 6 checks that the run of memory holding
# rsp is written FIRST, so a truncated dump keeps the stack. With the wrong
# register file the writer looks for the DUMPER's rsp, finds it in none of the
# dying process's regions, and writes the segments in plain address order --
# so the dump loses the stack as well as the registers. The control was not
# written to demonstrate that; it fell out, and it is why those two checks
# carry the REGFILE prefix.
#
# What stays green is the half that makes this a control rather than a switch
# that breaks things: the file is still a well-formed ELF core, readelf still
# counts four LOAD segments and finds all five notes, gdb still opens it and
# still reports SIGSEGV and the program name, cr2/err/trapno and NT_SIGINFO's
# si_addr are all still right (a signal handler DOES receive those correctly,
# which is why the control deliberately leaves them alone), every dumped page is
# still byte-identical, and the whole region table is still right.
test-coredump-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(CORE_CF) -w -DCOREDUMP_SIGCTX -o $(BUILD)/coredump_test_sigctx \
	    $(CORE_SRC) $(CORE_INC)
	@if $(BUILD)/coredump_test_sigctx > $(BUILD)/coredump_negctl.log 2>&1; then \
	    echo "FAIL: the sig-context control PASSED -- the gate does not measure"; \
	    echo "      which register file ends up in the dump"; exit 1; fi
	@bad=$$(grep '^FAIL' $(BUILD)/coredump_negctl.log | grep -cv 'REGFILE'); \
	 if [ "$$bad" -ne 0 ]; then \
	    echo "FAIL: $$bad reddened check(s) are NOT register-file checks -- the"; \
	    echo "      control damages something other than the register file:"; \
	    grep '^FAIL' $(BUILD)/coredump_negctl.log | grep -v 'REGFILE'; exit 1; fi
	@n=$$(grep -c '^FAIL' $(BUILD)/coredump_negctl.log); \
	 if [ "$$n" -lt 8 ] || [ "$$n" -gt 19 ]; then \
	    echo "FAIL: $$n REGFILE checks reddened, outside the 8..19 measured range"; \
	    grep '^FAIL' $(BUILD)/coredump_negctl.log; exit 1; fi; \
	 echo "     ($$n of the 19 REGFILE checks reddened; see the note above on why"; \
	 echo "      that is a range and rbx may survive)"
	@for k in "FAIL: REGFILE rip" "FAIL: REGFILE rsp" \
	          "FAIL: REGFILE rax" "FAIL: REGFILE rdi" \
	          "FAIL: REGFILE coredump_read_gregs" \
	          "FAIL: REGFILE gdb: rip" "FAIL: REGFILE gdb: rsp" \
	          "FAIL: REGFILE a truncated dump"; do \
	    grep -q "$$k" $(BUILD)/coredump_negctl.log || \
	      { echo "FAIL: the control did not redden '$$k', which cannot coincide"; \
	        exit 1; }; \
	 done
	@for k in "ok  : readelf: Type is CORE" "ok  : gdb: terminated with SIGSEGV" \
	          "ok  : LOGIT cr2" "ok  : every dumped page holds the bytes"; do \
	    grep -q "$$k" $(BUILD)/coredump_negctl.log || \
	      { echo "FAIL: the control broke '$$k', which it must not touch"; exit 1; }; \
	 done
	@echo "ok: the sig-context control reddens only register-file checks, including"
	@echo "    all eight that cannot coincide, and leaves the format intact"

# --- test-coredump-os ------------------------------------------------------
# ON THE MACHINE. See tests/boot/run-core-test.sh for what the two channels
# are; the short version is that the kernel prints the TRAP FRAME on its
# [fault] line and prints the FILE on its [core] line, the host then extracts
# the file off the real disk image and gives it to gdb, and the fault address
# came from fsroot/as/examples/crashme.as before the machine booted.
test-coredump-os: $(ISO) $(DISK) $(BUILD)/corecheck
	@bash tests/boot/run-core-test.sh $(ISO) $(DISK)

# ===========================================================================
# ptrace -- c/kernel/exec/ptrace.{c,h}. In this fragment rather than one of its
# own because it shares the register-order definition with the core dump
# (coredump.h's CORE_R15..CORE_GS is what GETREGS hands over) and because the
# two are halves of one question: a dump says what a program looked like when
# it died, ptrace says what it looks like while it is alive.
#
# NO HOST GATE, and that is a decision rather than an omission. Every part of
# ptrace that can be wrong is a property of the real machine -- whether a
# ring-3 loop reaches the stop in ksigframe.c inside a timer tick, whether the
# saved trap frame is the tracee's, whether walking another process's page
# table by hand lands on the right physical frame. A host model of those is a
# model of the answer. The core dump's builder is a pure function over bytes
# and is host-testable for exactly the opposite reason.
#
# ALSO NOT HERE, and named in c/kernel/exec/ptrace.h rather than left to be
# discovered: no stop-on-fatal-signal, no single-step, no TRACEME. All three
# need the tracer to be WOKEN and waitpid() to learn to report a ptrace stop,
# which is a second state machine across proc.c and ksignal.c.
.PHONY: test-ptrace-os test-ptrace-negctl

test-ptrace-os: $(ISO) $(DISK)
	@bash tests/boot/run-ptrace-test.sh $(ISO) $(DISK)

# --- test-ptrace-negctl ----------------------------------------------------
# -DPTRACE_NO_OWNER_CHECK removes both ownership rules: the uid test at ATTACH
# and, more importantly, the "you must be the tracer" test on every other
# request. That is the shape a first implementation has -- a pid is treated as
# a capability -- and NOTHING ELSE CHANGES: attach still works, registers are
# still read correctly, the tracer's own verdict is still PTRACE-OK. The only
# difference is that the third process, which attached to nothing, can now read
# a stopped process's registers.
#
# So the control is asserted on the INTRUDER'S COUNT and not on a pass/fail:
# `PTINTRUDE readable` must go from 0 to non-zero. A control checked by "the
# suite goes red" would also be satisfied by a -D that broke the build.
#
# It builds a whole second kernel and ISO into $(BUILD)/negctl-ptrace/ rather
# than rebuilding in place: the tree is shared and a control that leaves a
# crippled kernel behind is the thing everyone else then measures.
PTNEG_DIR := $(BUILD)/negctl-ptrace
test-ptrace-negctl:
	@mkdir -p $(PTNEG_DIR)
	@echo "building a kernel with -DPTRACE_NO_OWNER_CHECK into $(PTNEG_DIR)"
	@$(MAKE) --no-print-directory BUILD=$(PTNEG_DIR) \
	    CFLAGS='$(CFLAGS) -DPTRACE_NO_OWNER_CHECK' $(PTNEG_DIR)/logit.iso
	@WANT_INTRUDE=1 bash tests/boot/run-ptrace-test.sh \
	    $(PTNEG_DIR)/logit.iso $(DISK)

# NAMED ON THE POSITIVE, and this line is the whole fix for one entry of the
# STRANDED CONTROLS category. `tools/audit_tests.py` reported test-ptrace-negctl
# as STRANDED (NEW) on 2026-08-21 -- NOT_CI drops every `test-*-negctl` from
# suite discovery on the ground that its positive counterpart runs it, and
# nothing here ran it, so it was excluded from CI AND invoked from nowhere: run
# never, while looking exactly like a control that is covered. Re-run of the
# audit after this line: the STRANDED (NEW) list is empty.
#
# It costs a second kernel build (~15 min cold, incremental after), which is the
# same bill tests/sched.mk pays for the same reason and accepted there in
# writing. The alternative -- naming it on a ci-boot: line beside the positive --
# satisfies UNWIRED and still runs it never, which is worse because it looks
# fixed.
test-ptrace-os: test-ptrace-negctl
