# Targets for the EXECUTABLE LOADER -- c/kernel/exec/{elf,aex}.c. Included from
# the top-level Makefile.
#
# The loader parses disk-controlled input in ring 0 and then decides what ring 3
# is allowed to write and execute. Both of those make it worth testing at a
# different standard from "the machine still boots", so there are five kinds of
# test here and they answer different questions:
#
#   test-exec          every binary this tree builds still loads, byte for byte,
#                      with the permissions p_flags asked for -- plus the
#                      rejection matrix and the per-header behaviour.
#   test-exec-fuzz     the parser survives mutated input and never maps outside
#                      the user region. This is the one that found the
#                      p_memsz = -0x1000 overflow.
#   test-exec-negctl   two deliberately broken loaders that the tests above MUST
#                      fail against, on the right lines.
#   test-exec-asan     the same battery under ASan + UBSan.
#   test-exec-os       ON THE MACHINE: every program under /bin is fork+exec'd
#                      by the real shell and has to run.
#   test-bigexec       HOW BIG a program the machine can load -- 16/32/64 MiB,
#                      on a fresh machine and again on a churned one, with
#                      test-bigexec-negctl running the pre-streaming loader on
#                      the SAME machine so the difference cannot be read as
#                      "you gave one of them less memory".
#
# The host tests link c/kernel/exec/elf.c and aex.c UNMODIFIED and replace only
# the machine under them (tests/unit/exechost/space.c), which uses the host MMU
# -- so "this page is not writable" is measured by storing to it and catching
# the fault, not by reading back a flag the test itself wrote.

.PHONY: test-exec test-exec-fuzz test-exec-negctl test-exec-asan test-exec-os \
        test-exec-bases

EXEC_SRC  := tests/unit/exechost/space.c c/kernel/exec/elf.c c/kernel/exec/aex.c \
             c/drivers/block/crc32.c
EXEC_INC  := -Itests/unit/exechost -Ic/kernel/exec -Ic/drivers/block -DLOGIT_HOSTTEST
EXEC_WARN := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
# Every .aex the build produces, including the deliberately crippled variants a
# test packs instead of the real app -- those are built by the same rules and
# are just as much "a binary that has to load".
EXEC_BINS  = $(wildcard $(BUILD)/*.aex)
# Seeds for the fuzzer: small ones so an iteration is cheap, but real ones, plus
# a GUI app and a mini-libc program so the segment shapes differ.
EXEC_SEEDS = $(BUILD)/echo.aex $(BUILD)/true.aex $(BUILD)/sh.aex \
             $(BUILD)/clock.aex $(BUILD)/ls.aex $(BUILD)/settings.aex

# --- the two files the container tests need, which no app build produces -----
#
# exec_v1.aex is the OLD FORMAT: the 64-byte v1 wrapper with an ELF at a
# hardcoded +64, no size, no architecture and no integrity record. It is the
# negative control for the format change -- an old file must be refused or
# migrated DELIBERATELY, and the deliberate part is that the loader says so on
# the log. Nothing but this rule builds one.
$(BUILD)/exec_v1.aex: $(BUILD)/echo.aex tools/mkaex.py $(BUILD)/echo.elf
	@python3 tools/mkaex.py --v1 $(BUILD)/echo.elf $@ echo - '*' 150 150 150 > /dev/null

# asnative.aex is a .aex a COMPILER could have emitted: nasm produces a FLAT
# binary -- no ELF, no sections, no symbols, just bytes and an entry offset,
# which is what a code generator has -- and `mkaex.py --emit` builds the ELF64
# and the container around it. This is the answer to "design the format so a
# compiler can emit it, not only a linker", and it is checked twice: host-side
# by test-exec, and by actually running it on the machine in test-exec-os.
$(BUILD)/asnative.bin: tests/unit/asnative.asm
	@mkdir -p $(BUILD)
	@$(ASM) -f bin tests/unit/asnative.asm -o $@
$(BUILD)/asnative.aex: $(BUILD)/asnative.bin tools/mkaex.py
	@python3 tools/mkaex.py --emit $@ asnative - '*' --cli --category test \
	    --base 0x50000000 --text $(BUILD)/asnative.bin --id os.logit.asnative > /dev/null

EXEC_FIXTURES := $(BUILD)/exec_v1.aex $(BUILD)/asnative.aex
# The container tests reach these by name rather than by argv position: the
# binary list is a glob, and a fixture that has to be argv[1] is a fixture one
# `ls` away from testing the wrong file.
EXEC_ENV = EXEC_V1=$(BUILD)/exec_v1.aex EXEC_V2=$(BUILD)/echo.aex \
           EXEC_EMIT=$(BUILD)/asnative.aex

$(BUILD)/exec_test: tests/unit/exec_test.c $(EXEC_SRC) tests/unit/exechost/space.h \
                    c/kernel/exec/elf.h c/kernel/exec/aex.h
	@$(CC) -O1 -g $(EXEC_WARN) $(EXEC_INC) -o $@ tests/unit/exec_test.c $(EXEC_SRC)

$(BUILD)/exec_fuzz: tests/unit/exec_fuzz.c $(EXEC_SRC) tests/unit/exechost/space.h \
                    c/kernel/exec/elf.h c/kernel/exec/aex.h
	@$(CC) -O1 -g $(EXEC_WARN) $(EXEC_INC) -o $@ tests/unit/exec_fuzz.c $(EXEC_SRC)

# --- test-exec -------------------------------------------------------------
# $(AEX) is a prerequisite so the binaries exist; the test then globs the build
# directory, which also picks up the crippled variants other tests pack.
#
# THE CONTROLS RUN WITH IT, as a prerequisite rather than as a recipe line.
# tools/audit_tests.py deliberately excludes `test-*-negctl` from suite
# discovery -- "negative controls that are RUN BY their parent" -- so a control
# whose parent does not run it is reached by nothing at all, which is the exact
# shape that audit exists to find. Written as a prerequisite for the reason
# tests/loader.mk gives: a recipe line can be lost to a whole-file overwrite of
# the Makefile, and a hook that can be lost is a hook that will be.
test-exec: test-exec-negctl
test-exec: $(BUILD)/exec_test $(AEX) $(EXEC_FIXTURES)
	@$(EXEC_ENV) $(BUILD)/exec_test $(EXEC_BINS)

# --- test-exec-fuzz --------------------------------------------------------
# Three fixed seeds rather than a random one: a fuzz target whose corpus changes
# every run cannot be bisected, and a failure that cannot be reproduced is a
# rumour. 40k iterations each is a few seconds.
test-exec-fuzz: $(BUILD)/exec_fuzz $(AEX)
	@for s in 1 2 3; do \
	    $(BUILD)/exec_fuzz 40000 $$s $(EXEC_SEEDS) || exit 1; \
	done

# --- test-exec-negctl ------------------------------------------------------
# CONTROL 1: the overflow check as it was written before the fuzzer got to it
# (`end < start`). p_memsz = -0x1000 with an unaligned p_vaddr makes the rounded
# end EQUAL start, so that check passes, nothing is mapped, and the loader
# memcpys to an unmapped address.
#
# WHAT THE CONTROL ACTUALLY DOES, because it is not "prints FAIL": it takes the
# fault. The harness maps with the host MMU, so a write to an address the loader
# never mapped is a real SIGSEGV -- which is exactly what it would be in ring 0,
# and is the reason this is worth a control at all. So the assertion is shaped
# to that: the run must not pass, it must have REACHED the case (the check
# immediately before it passed), and it must not have got through it.
#
# The FUZZER must also not finish cleanly. A fuzz target that passes against the
# bug it found is a fuzz target that is not running.
#
# CONTROL 2: p_flags ignored, every page mapped writable, i.e. the loader before
# 37a0849. The unit test must fail on the per-segment permission checks.
test-exec-negctl: $(AEX)
	@$(CC) -O1 -g -w $(EXEC_INC) -DELF_NEGCTL_OVERFLOW -o $(BUILD)/exec_test_ovf \
	    tests/unit/exec_test.c $(EXEC_SRC)
	@if $(BUILD)/exec_test_ovf $(BUILD)/echo.aex > $(BUILD)/exec_negctl_ovf.log 2>&1; then \
	    echo "FAIL: the overflow negative control PASSED -- the unit test does not measure the fix"; \
	    exit 1; fi
	@grep -q 'ok: p_vaddr + p_memsz overflowing' $(BUILD)/exec_negctl_ovf.log || \
	    { echo "FAIL: the control never reached the overflow case"; exit 1; }
	@if grep -q 'ok: p_memsz = -0x1000' $(BUILD)/exec_negctl_ovf.log; then \
	    echo "FAIL: the control survived the very case it is supposed to fail on"; exit 1; fi
	@grep -q 'ok: the reference image loads' $(BUILD)/exec_negctl_ovf.log || \
	    { echo "FAIL: the control broke the loader outright, not just the overflow check"; exit 1; }
	@$(CC) -O1 -g -w $(EXEC_INC) -DELF_NEGCTL_OVERFLOW -o $(BUILD)/exec_fuzz_ovf \
	    tests/unit/exec_fuzz.c $(EXEC_SRC)
	@if $(BUILD)/exec_fuzz_ovf 40000 1 $(EXEC_SEEDS) > $(BUILD)/exec_negctl_fuzz.log 2>&1; then \
	    echo "FAIL: the fuzzer PASSED against the overflow bug it found"; exit 1; fi
	@echo "ok: the overflow control fails the unit test on the right case, and the fuzzer does not survive it"
	@$(CC) -O1 -g -w $(EXEC_INC) -DELF_NEGCTL_NOWX -o $(BUILD)/exec_test_nowx \
	    tests/unit/exec_test.c $(EXEC_SRC)
	@if $(BUILD)/exec_test_nowx $(BUILD)/sh.aex > $(BUILD)/exec_negctl_nowx.log 2>&1; then \
	    echo "FAIL: the W^X negative control PASSED -- the test does not measure W^X"; \
	    exit 1; fi
	@grep -q 'writable=1, p_flags says 0' $(BUILD)/exec_negctl_nowx.log || \
	    { echo "FAIL: the W^X control failed, but not on a segment permission"; exit 1; }
	@grep -q 'ok: the reference image loads' $(BUILD)/exec_negctl_nowx.log || \
	    { echo "FAIL: the W^X control broke loading itself"; exit 1; }
	@echo "ok: the W^X control fails on the segment permissions, and only on those"

# --- test-exec-asan --------------------------------------------------------
# The loader writes to addresses it computed from the file. UBSan catches the
# arithmetic that produced them and ASan catches the reads out of the image
# buffer -- neither of which the MMU-backed harness can see, because a read
# past the end of a malloc'd image is still a valid host address.
test-exec-asan: $(AEX)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(EXEC_INC) -o $(BUILD)/exec_test_asan tests/unit/exec_test.c $(EXEC_SRC)
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/exec_test_asan $(EXEC_SEEDS)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    $(EXEC_INC) -o $(BUILD)/exec_fuzz_asan tests/unit/exec_fuzz.c $(EXEC_SRC)
	@ASAN_OPTIONS=detect_leaks=0 $(BUILD)/exec_fuzz_asan 8000 7 $(EXEC_SEEDS)

# --- test-exec-bases -------------------------------------------------------
# The link-base map, checked instead of remembered. Every GUI app links at a
# distinct fixed base because nothing relocates; the map runs 0x40000000 to
# 0x4B000000 and the consequence of two apps sharing a base is not a link error
# but a launch that quietly loads the wrong image. Deliberate sharing (a
# crippled variant packed in place of the app it replaces) is allowed -- it is
# never on the disk at the same time as its original.
test-exec-bases: $(AEX)
	@python3 tests/unit/exec_bases.py $(EXEC_BINS)

# --- test-exec-os ----------------------------------------------------------
# ON THE MACHINE. The host battery proves the loader accepts the bytes; this
# proves the process it builds actually runs -- the auxv is on the stack the
# program was handed, the entry point is reachable, the read-only segments are
# readable and the writable ones are writable, under the real MMU in ring 3.
test-exec-os: $(ISO) $(DISK)
	@bash tests/boot/run-exec-test.sh $(ISO) $(DISK)

# --- test-bigexec ----------------------------------------------------------
# HOW BIG A PROGRAM THIS MACHINE CAN LOAD, on the machine, with a control.
#
# The question is not academic and it is not about compilers: `cc1` is 35.7 MB
# dynamically linked and 50-60 MB static, so ONE file of a toolchain is larger
# than every .aex on this disk put together. Before the streaming loader the
# ceiling was exec.c's `kmalloc(whole file)`, and the shape of that ceiling is
# what makes a gate worth writing -- it is not "you run out of memory", it is
# kheap's grow() DOUBLING an arena and then asking pmm_alloc_contig() for one
# contiguous physical run. So the failure lands at the next power of two above
# the file, in one piece, with most of the machine still free.
#
# Three sizes and TWO phases in one boot. The second phase is the one that
# matters: pmm_alloc_contig is a linear first-fit with no fallback, so a load
# that works on a machine that has just booted says nothing about the same load
# after the desktop, the page cache and a browser-shaped workload have used and
# released memory. Phase 2 runs the identical three binaries after that churn.
#
# Every number the harness asserts on is printed by the guest: the pad program
# prints the byte count it actually touched and the SUM of three planted bytes
# (so a wrong-bytes load fails, not just a crash), and the kernel prints its own
# frame counters. Nothing is inferred from "it did not hang".
BIGEXEC_SIZES := 16 32 64
BIGEXEC_DIR   := $(BUILD)/bigexec

# The blob, the object, the ELF, the container. Written out per size rather than
# through a $(foreach) template because the .incbin path has to reach the
# assembler as a -D and a template hides that.
define BIGEXEC_RULE
$(BIGEXEC_DIR)/pad-$(1).bin: tests/unit/bigexec_gen.py
	@mkdir -p $(BIGEXEC_DIR)
	@python3 tests/unit/bigexec_gen.py $$$$(( $(1) * 1024 * 1024 )) $$@
$(BIGEXEC_DIR)/pad-$(1).elf: $(BIGEXEC_DIR)/pad-$(1).bin tests/unit/bigexec_pad.c \
                             tests/unit/bigexec_pad.S $(APPDIR)/crt0_cli.asm
	@$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BIGEXEC_DIR)/crt0c-$(1).o
	@$(CC) $(UCFLAGS) -c tests/unit/bigexec_pad.c -o $(BIGEXEC_DIR)/pad-$(1).o
	@$(CC) $(UCFLAGS) -DPADFILE='"$(BIGEXEC_DIR)/pad-$(1).bin"' \
	    -c tests/unit/bigexec_pad.S -o $(BIGEXEC_DIR)/blob-$(1).o
	@$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $$@ \
	    $(BIGEXEC_DIR)/crt0c-$(1).o $(BIGEXEC_DIR)/pad-$(1).o $(BIGEXEC_DIR)/blob-$(1).o
$(BIGEXEC_DIR)/pad-$(1).aex: $(BIGEXEC_DIR)/pad-$(1).elf tools/mkaex.py
	@python3 tools/mkaex.py $$< $$@ pad$(1) - '*' --cli --category test \
	    --id os.logit.bigexec$(1) > /dev/null
endef
$(foreach s,$(BIGEXEC_SIZES),$(eval $(call BIGEXEC_RULE,$(s))))

BIGEXEC_AEX := $(foreach s,$(BIGEXEC_SIZES),$(BIGEXEC_DIR)/pad-$(s).aex)
BIGEXEC_MAP := $(foreach s,$(BIGEXEC_SIZES),$(BIGEXEC_DIR)/pad-$(s).aex:/bin/pad$(s))

# The image. NOT $(DISK): the pads are 112 MiB of test fixture and putting them
# on the disk every other harness boots would slow every one of them down and
# change what the page cache holds under tests that measure it.
$(BIGEXEC_DIR)/disk.img: $(BIGEXEC_AEX) $(DISK) tests/unit/bigexec_img.py tools/mkfs.py
	@mkdir -p $(BIGEXEC_DIR)
	@$(MAKE) -n $(DISK) > $(BIGEXEC_DIR)/make-n.txt
	@python3 tests/unit/bigexec_img.py . $(BIGEXEC_DIR)/make-n.txt $@ $(BIGEXEC_MAP)

test-bigexec: $(ISO) $(BIGEXEC_DIR)/disk.img
	@bash tests/boot/run-bigexec.sh $(ISO) $(BIGEXEC_DIR)/disk.img

# --- test-bigexec-negctl ---------------------------------------------------
# The whole-file materialisation, restored on a -D (EXEC_NEGCTL_SLURP in
# c/kernel/exec/exec.c). It is the PLAUSIBLE wrong implementation -- the one
# that shipped, and one that loads every ordinary program on this disk -- so the
# gate demands BOTH halves: the 64 MiB pad must fail against it, and the 16 MiB
# pad must still pass. A control that fails at every size is measuring "did I
# break the loader", not the ceiling this work exists to remove.
#
# It needs its own kernel, hence its own ISO, and the ISO rule has no -D hook --
# so the object is rebuilt in place, the ISO relinked, and the tree's own
# `make $(ISO)` restores it afterwards. The restore is in a trap: leaving a
# crippled kernel in build/ would poison every later harness in the sweep.
test-bigexec-negctl: $(ISO) $(BIGEXEC_DIR)/disk.img
	@bash tests/boot/run-bigexec-negctl.sh $(BIGEXEC_DIR)/disk.img

.PHONY: test-bigexec test-bigexec-negctl
# The control runs WITH its positive, as a PREREQUISITE and never as a recipe
# line -- see the note above test-exec, and tests/audit-stranded.baseline for
# what naming it on a `ci-boot:` line instead would buy: it would satisfy the
# UNWIRED audit and still run the control never.
test-bigexec: test-bigexec-negctl
# Two QEMU boots and 112 MiB of fixture, so this is a boot suite, not a host one.
ci-boot: test-bigexec

# poll(), eventfd and timerfd -- c/kernel/exec/kpoll.c and kpollsys.c. Its own
# fragment, included from here rather than from the top-level Makefile, because
# the Makefile is contended and `-include` nests. See the header of tests/poll.mk.
-include tests/poll.mk

# /proc -- c/fs/procfs.c + procfs_src.c, and /bin/{ps,free,uptime}. Its own
# fragment, included from here rather than from the top-level Makefile, because
# the Makefile is contended and `-include` nests. See the header of
# tests/procfs.mk.
-include tests/procfs.mk

# Core dumps -- c/kernel/exec/coredump.c + c/apps/coreutils/corefmt.h. Its own
# fragment, included from here rather than from the top-level Makefile, for the
# reason tests/poll.mk and tests/procfs.mk both give: the Makefile is contended
# and `-include` nests. See the header of tests/coredump.mk.
-include tests/coredump.mk

# How big the filesystem IS -- the image geometry gate and its drift control.
# Its own fragment, included from here rather than from the top-level Makefile,
# for the reason tests/poll.mk, tests/procfs.mk and tests/coredump.mk all give:
# the Makefile is contended and `-include` nests. It hangs off exec.mk because
# the image was grown to hold a PROGRAM this loader could not otherwise be given
# -- one file of a C toolchain did not fit in the whole 64 MiB filesystem. See
# the header of tests/fsgeom.mk.
-include tests/fsgeom.mk

# --- the argv / line / name LIMITS, and what happens one past each -----------
#
# Four silent truncations, measured on device 2026-08-20 and all fixed the
# same way: the limit is raised to what a toolchain needs, and one past it is
# a LOUD refusal that does not run the command. The places:
#   /bin/sh                 c/apps/coreutils/sh.c  (words per command, bytes per line,
#                           glob expansion, expanded-argv arena)
#   the kernel's execve     c/kernel/exec/exec.c copy_uvec (LOGIT_EXEC_E2BIG)
#   the file-name limit     c/fs/vfs_path.h now DERIVES VFS_NAME_MAX from the on-disk
#                           LFS_NAME_MAX instead of carrying its own 60
# include/abi/logit_exec.h is the one definition both ends of execve read.
#
# Three gates, each with a negative control that is the code AS IT SHIPPED on
# a -D switch, and each control must redden EXACTLY the checks that look for a
# refusal -- the checks that exercise a command AT the limit have to keep
# passing against it, or the suite is measuring "did it break" rather than
# "does it refuse". The expected counts are pinned below; a control that
# reddens more or fewer is a changed test, not a passing one.
.PHONY: test-sh-limits test-sh-limits-negctl test-namemax test-namemax-negctl \
        test-argv-limits-os

SH_LIMITS_INC := -Itests/unit -Ic/apps/coreutils -Ic/apps -Iinclude/abi
SH_LIMITS_NEGCTL_REDDENS := 11
NAMEMAX_NEGCTL_REDDENS   := 3

# The REAL sh.c against the host stub, exactly as test-sh builds it.
test-sh-limits: test-sh-limits-negctl
test-sh-limits:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/sh_limits_test tests/unit/sh_limits_test.c $(SH_LIMITS_INC)
	@$(BUILD)/sh_limits_test

test-sh-limits-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DSH_LIMITS_NEGCTL -o $(BUILD)/sh_limits_negctl tests/unit/sh_limits_test.c $(SH_LIMITS_INC)
	@if $(BUILD)/sh_limits_negctl > $(BUILD)/sh_limits_negctl.log 2>&1; then \
	    echo "FAIL: the silently-truncating shell PASSES the limits suite -- it measures nothing"; exit 1; fi
	@n=$$(grep -c '^FAIL' $(BUILD)/sh_limits_negctl.log); \
	 if [ "$$n" -ne $(SH_LIMITS_NEGCTL_REDDENS) ]; then \
	    echo "FAIL: the control must redden exactly $(SH_LIMITS_NEGCTL_REDDENS) refusal checks, got $$n:"; \
	    grep '^FAIL' $(BUILD)/sh_limits_negctl.log; exit 1; fi
	@if grep -q 'must tokenize whole\|must fork exactly once\|must be read whole\|two matches into two slots' $(BUILD)/sh_limits_negctl.log; then \
	    echo "FAIL: an at-the-limit check reddened under the control -- the control broke the shell, not just its refusals"; \
	    grep '^FAIL' $(BUILD)/sh_limits_negctl.log; exit 1; fi
	@echo "negative control ok: the silently-truncating shell fails exactly $(SH_LIMITS_NEGCTL_REDDENS) refusal checks and passes every at-the-limit one"

# The VFS walker and the REAL logitfs (on the simulated device the crash tests
# use) asked the same question at every length. FS_CFLAGS/FS_CORE/FS_STUB are
# the Makefile's own fs-host variables: recipe-time expansion, so the order of
# -include does not matter.
test-namemax: test-namemax-negctl
test-namemax:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -o $(BUILD)/namemax_test tests/unit/namemax_test.c $(FS_CORE) c/fs/vfs_path.c $(FS_STUB)
	@$(BUILD)/namemax_test

test-namemax-negctl:
	@mkdir -p $(BUILD)
	@$(CC) $(FS_CFLAGS) -DVFS_NAME_MAX_LEGACY -o $(BUILD)/namemax_negctl tests/unit/namemax_test.c $(FS_CORE) c/fs/vfs_path.c $(FS_STUB)
	@if $(BUILD)/namemax_negctl > $(BUILD)/namemax_negctl.log 2>&1; then \
	    echo "FAIL: the typed-60 VFS_NAME_MAX PASSES the name-limit suite -- the crack is not measured"; exit 1; fi
	@n=$$(grep -c 'FAIL:' $(BUILD)/namemax_negctl.log); \
	 if [ "$$n" -ne $(NAMEMAX_NEGCTL_REDDENS) ]; then \
	    echo "FAIL: the control must redden exactly $(NAMEMAX_NEGCTL_REDDENS) checks (both about length 60), got $$n:"; \
	    grep 'FAIL:' $(BUILD)/namemax_negctl.log; exit 1; fi
	@grep -q 'length 60: vfs accepts, logitfs refuses' $(BUILD)/namemax_negctl.log || \
	    { echo "FAIL: the control reddened, but not on the length-60 disagreement it exists to show"; exit 1; }
	@echo "negative control ok: with the typed 60 back, length 60 is accepted by the VFS and refused by logitfs, and nothing else moves"

# ON THE MACHINE: every limit at its bound and one past it, through the real
# /bin/sh over the serial console, the real filesystem, and a hand-built argv
# from /bin/as for the kernel's own bound (the shell cannot overshoot it).
test-argv-limits-os: $(ISO) $(DISK)
	@bash tests/boot/run-argv-limits-test.sh $(ISO) $(DISK)

ci-host: test-sh-limits test-namemax
ci-boot: test-argv-limits-os

# What an open file DESCRIPTION costs -- c/kernel/exec/file.c's F_VFS backend
# holds no bytes for a read-only open. Its own fragment, included from here
# rather than from the top-level Makefile, for the reason tests/poll.mk,
# tests/procfs.mk, tests/coredump.mk and tests/fsgeom.mk all give: the Makefile
# is contended and `-include` nests. It hangs off exec.mk because it is the same
# change one layer up from the streaming exec loader above -- test-bigexec asks
# how big a program can be LOADED, test-fdstream asks how big a file can be
# OPENED, and until now the second question had the smaller answer. See the
# header of tests/fdstream.mk.
-include tests/fdstream.mk
