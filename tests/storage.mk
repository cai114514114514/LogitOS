# tests/storage.mk -- the durable-storage primitive's gates (wave 1, 2026-08-30).
# Owned by the storage-kernel agent; the root Makefile's `-include
# tests/storage.mk` line was pre-wired by commit 1461cd173, so this fragment
# fills in targets and never touches the Makefile.
#
# WHAT LANDED, AND THE MEASUREMENT THAT DECIDED IT
# -------------------------------------------------
# js_idb.c:33 and js_cache.c:58 refuse durable storage with the same sentence:
# "there is no VFS positional write to build real durability on". Before
# building on it, ring 3's ACTUAL capabilities on the disk filesystem were
# measured on the machine (fsroot/as/examples/storprobe.as, driven by
# tests/boot/run-storage-probe.sh, 2026-08-30 13:14, 512M TCG -smp 4,
# persistent disk copy; the raw matrix is preserved in the SYS_FTRUNCATE block
# of include/abi/logit_abi.h and in the wave report):
#
#   open+O_TRUNC+full rewrite       WORKS   64/64 bytes verified at reopen
#   append (O_APPEND)               WORKS   byte-exact at the seam
#   lseek+write mid-file (O_RDWR)   WORKS   in-place, byte-exact -- so the fd
#                                           layer's "positional write" EXISTS
#   grow by seek-past-end+write     BROKEN  10 of 16 gap bytes read back
#                                           NON-ZERO: uninitialized kernel
#                                           heap persisted into a file ring 3
#                                           can read (also an info leak)
#   shrink                          ABSENT  no syscall; libc ftruncate()
#                                           refuses it with ENOSYS
#   rename over an existing file    REFUSED logitfs_rename is no-clobber, so
#                                           temp+rename atomic replace is out
#   fsync / failed-write reporting  WORKS   SYS_FSYNC; SYS_CLOSE returns -2
#                                           when the flush failed
#
# So the quoted sentence was stale in both directions, and the two real gaps
# closed here are: (1) SYS_FTRUNCATE -- the length change a store that shrank
# needs (a deleted cookie, a cleared key); (2) the hole rule -- a write past
# EOF now zero-fills the gap, which fixes the leak AND makes libc's grow-by-
# seek-past-end honest. pwrite and rename-replace were REJECTED, with reasons,
# in the SYS_FTRUNCATE block of include/abi/logit_abi.h.
#
# THE THREE TARGETS
# -----------------
#   test-storage          HOST unit: the REAL c/kernel/exec/fd/file.c against
#                         tests/unit/storhost/hostmodel.c (what is real and
#                         what is modelled is that file's header -- the short
#                         version: file.c entire, the VFS by its documented
#                         contract, everything else stubbed to abort).
#                         Covers grow-zero-fill, shrink, flush-at-new-length,
#                         the no-op no-write-back trap, every refusal, the
#                         refused-flush report, and the whole-store pattern.
#   test-storage-negctl   the control: the SAME sources built
#                         -DSTORAGE_NEGCTL, which restores BOTH pre-fix
#                         behaviours inside file.c (hole left as allocated,
#                         truncate answering -1). The suite must FAIL under
#                         it -- for exactly the hole/truncate reasons and
#                         nothing else -- or the asserts are attached to
#                         nothing. A PREREQUISITE of both positives, per the
#                         stranded-controls rule: a control named on a ci-
#                         line runs never while looking covered.
#   test-storage-os       ON THE MACHINE, two boots against ONE persistent
#                         disk copy (no -snapshot): boot 1 runs
#                         fsroot/as/examples/storgate.as phase "run"
#                         (write -> rewrite shorter -> ftruncate grow ->
#                         ftruncate shrink -> mid-file lseek+write -> write
#                         past EOF -> whole-store image + refusal checks,
#                         every step re-read and byte-verified), boot 2 phase
#                         "verify" re-reads the image byte-for-byte. A write
#                         that only lived in boot 1's kernel buffer cannot
#                         pass boot 2; only the disk can.
#
# THE RED SIDE. The host control is watchable any time (-DSTORAGE_NEGCTL);
# it was watched RED on 2026-08-30 with exactly 8 failing checks -- the two
# ftruncate length checks, the two hole/gap-zero checks, and the three
# reopen/flush checks that ride them -- while every refusal check and the
# whole-store pattern still passed, i.e. the control breaks only what the
# fix fixed. The on-device red is the PRE-FIX probe run above: the same
# asserts, as storprobe.as against the unmodified kernel, answered
# "ftruncate rc -1" and "hole-nonzero-bytes 10" -- recorded here because an
# on-device control would cost two boots to restate what the host control
# proves deterministically every run.
#
#   test-storage-probe    the step-0 instrument itself, re-runnable by name
#                         (asserts NOTHING; it is the before/after matrix
#                         tool, not a gate -- a probe that must pass is a
#                         gate wearing a probe's name). Not on any ci line.

.PHONY: test-storage test-storage-negctl test-storage-os test-storage-probe

# The real code under test is ONE translation unit (file.c); the pollhost
# pattern's subtraction-from-C_SRC has nothing to subtract, so the three
# sources are named. -iquote and not -I for poll.mk's reason: file.c includes
# "sched.h" and "pit.h", whose basenames collide with the host's own headers
# under a flat -I scan, and this TU must see the kernel's.
STORAGE_HOST_SRC := tests/unit/storage_test.c tests/unit/storhost/hostmodel.c \
                    c/kernel/exec/fd/file.c
STORAGE_HOST_INC := -iquote c $(KEXEC_IQ) $(KMM_IQ) \
                    $(KCORE_IQ) $(KCPU_IQ) \
                    -iquote c/kernel/sched -iquote c/fs $(FS_IQ) \
                    -iquote c/drivers/char -iquote c/drivers/timer \
                    -iquote include/abi
# ASan + UBSan: file.c grows kmalloc buffers and does offset arithmetic on
# longs a hostile caller sizes (off + len overflow guards); those failures are
# silent by nature and this is the cheapest instrument that can see them.
STORAGE_HOST_CF  := -O1 -g -Wall -Wextra -fsanitize=address,undefined

$(BUILD)/storage_test: $(STORAGE_HOST_SRC) c/kernel/exec/fd/file.h c/fs/vfs/vfs.h \
                       include/abi/logit_abi.h
	@mkdir -p $(BUILD)
	$(CC) $(STORAGE_HOST_CF) -o $@ $(STORAGE_HOST_SRC) $(STORAGE_HOST_INC)

$(BUILD)/storage_test_negctl: $(STORAGE_HOST_SRC) c/kernel/exec/fd/file.h \
                              c/fs/vfs/vfs.h include/abi/logit_abi.h
	@mkdir -p $(BUILD)
	$(CC) $(STORAGE_HOST_CF) -DSTORAGE_NEGCTL -o $@ $(STORAGE_HOST_SRC) $(STORAGE_HOST_INC)

test-storage: $(BUILD)/storage_test
	@$(BUILD)/storage_test

test-storage-negctl: $(BUILD)/storage_test_negctl
	@echo "--- storage negative control: hole left as allocated, truncate absent ---"
	@if $(BUILD)/storage_test_negctl; then \
	    echo "NEGCTL FAILED TO FAIL: nothing the suite asserts depends on the fix"; exit 1; \
	 else \
	    echo "negctl reddened as required (the FAIL lines above are the proof)"; \
	 fi

# The control runs with the positive, as a prerequisite and never a recipe
# line -- the stranded-controls rule this tree paid to learn.
test-storage: test-storage-negctl

test-storage-os: $(ISO) $(DISK)
	@bash tests/boot/run-storage-test.sh $(ISO) $(DISK)
# The host control also runs with every on-device gate: two boots is too much
# to pay for re-stating it, but leaving it out would let the pair rot apart.
test-storage-os: test-storage-negctl

test-storage-probe: $(ISO) $(DISK)
	@bash tests/boot/run-storage-probe.sh $(ISO) $(DISK)

# Host gate is offline and deterministic -- ci-host. The on-device gate is a
# two-boot suite, the same price tests/fdstream.mk puts on ci-boot, and for
# the same reason: it is the only gate that can see the disk itself.
ci-host: test-storage
ci-boot: test-storage-os
