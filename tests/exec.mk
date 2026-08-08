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

EXEC_SRC  := tests/unit/exechost/space.c c/kernel/exec/elf.c c/kernel/exec/aex.c
EXEC_INC  := -Itests/unit/exechost -Ic/kernel/exec -DLOGIT_HOSTTEST
EXEC_WARN := -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
# Every .aex the build produces, including the deliberately crippled variants a
# test packs instead of the real app -- those are built by the same rules and
# are just as much "a binary that has to load".
EXEC_BINS  = $(wildcard $(BUILD)/*.aex)
# Seeds for the fuzzer: small ones so an iteration is cheap, but real ones, plus
# a GUI app and a mini-libc program so the segment shapes differ.
EXEC_SEEDS = $(BUILD)/echo.aex $(BUILD)/true.aex $(BUILD)/sh.aex \
             $(BUILD)/clock.aex $(BUILD)/ls.aex $(BUILD)/settings.aex

$(BUILD)/exec_test: tests/unit/exec_test.c $(EXEC_SRC) tests/unit/exechost/space.h \
                    c/kernel/exec/elf.h c/kernel/exec/aex.h
	@$(CC) -O1 -g $(EXEC_WARN) $(EXEC_INC) -o $@ tests/unit/exec_test.c $(EXEC_SRC)

$(BUILD)/exec_fuzz: tests/unit/exec_fuzz.c $(EXEC_SRC) tests/unit/exechost/space.h \
                    c/kernel/exec/elf.h c/kernel/exec/aex.h
	@$(CC) -O1 -g $(EXEC_WARN) $(EXEC_INC) -o $@ tests/unit/exec_fuzz.c $(EXEC_SRC)

# --- test-exec -------------------------------------------------------------
# $(AEX) is a prerequisite so the binaries exist; the test then globs the build
# directory, which also picks up the crippled variants other tests pack.
test-exec: $(BUILD)/exec_test $(AEX)
	@$(BUILD)/exec_test $(EXEC_BINS)

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
# memcpys to an unmapped address. Two requirements, and both matter:
#   - the unit test must FAIL, and on that one named case (if it failed on
#     something else, the case is not the thing being measured);
#   - the FUZZER must not finish cleanly. It is allowed to crash -- that is the
#     bug -- but it may not pass. A fuzz target that passes against the bug it
#     was written for is a fuzz target that is not running.
#
# CONTROL 2: p_flags ignored, every page mapped writable, i.e. the loader before
# 37a0849. The unit test must fail on the per-segment permission checks.
test-exec-negctl: $(AEX)
	@$(CC) -O1 -g -w $(EXEC_INC) -DELF_NEGCTL_OVERFLOW -o $(BUILD)/exec_test_ovf \
	    tests/unit/exec_test.c $(EXEC_SRC)
	@if $(BUILD)/exec_test_ovf $(BUILD)/echo.aex > $(BUILD)/exec_negctl_ovf.log 2>&1; then \
	    echo "FAIL: the overflow negative control PASSED -- the unit test does not measure the fix"; \
	    exit 1; fi
	@grep -q 'FAIL: p_memsz = -0x1000' $(BUILD)/exec_negctl_ovf.log || \
	    { echo "FAIL: the control failed, but not on the overflow case"; exit 1; }
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
