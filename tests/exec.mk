# Targets for the EXECUTABLE LOADER -- c/kernel/exec/{elf,aex}.c. Included from
# the top-level Makefile.
#
# The loader parses disk-controlled input in ring 0 and then decides what ring 3
# is allowed to write and execute. Both of those make it worth testing at a
# different standard from "the machine still boots", so there are four kinds of
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
