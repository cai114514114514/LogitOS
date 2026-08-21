# Loadable kernel modules -- test targets.
#
# In its own fragment rather than in the Makefile because several agents are
# editing the Makefile at the same time. The Makefile already pulls it in --
# `-include tests/module.mk` is at Makefile:4248, in the block whose own
# comment records that six fragments once existed with no include line between
# them and make, so every gate they defined was unreachable and nothing said
# so.
#
# Nothing else is needed to BUILD the loader: C_SRC globs c/kernel, so
# c/kernel/module/{modelf,modload,ksyms}.c compile and link with no other
# Makefile change. Verified: build/c/kernel/module/*.o appear in the kernel
# link line.
#
# WHAT THE MAKEFILE DOES NEED, exactly two lines, and only for the BOOT gate
# (the host gates below need nothing). The module has to reach the guest
# filesystem, and the $(DISK) recipe is in the Makefile:
#
#     $(DISK): ... $(MOD_KO)              <- add $(MOD_KO) to the prerequisites
#     python3 tools/mkfs.py ... $(MOD_PACK) \    <- add $(MOD_PACK) to the recipe
#
# which is exactly the pattern tests/ch.mk already documents at its line 121
# ("recipes expand at execution time, so a variable a later -include defines is
# still visible"). Until those two lines exist, run-module-test.sh passes both
# on the make command line instead -- see the header of that script for the
# literal invocation and why a command-line override is equivalent here.

MOD_KO   := $(BUILD)/edu.ko
MOD_PACK := $(MOD_KO):/lib/modules/edu.ko

.PHONY: test-modreloc test-modreloc-negctl test-module test-module-negctl

# ci-host: these are seconds, no QEMU. The control is a PREREQUISITE of the
# positive rather than a sibling on the ci-host line, for the reason
# tests/virtio_rng.mk spells out: audit_tests.py's NOT_CI drops every
# `test-*-negctl` from what tools/ci.sh runs, on the ground that a control is
# run BY its positive counterpart -- so naming it beside the positive would
# exclude it and then invoke it from nowhere, which is the "stranded control"
# category CLAUDE.md counts 55 of.
ci-host: test-modreloc
test-modreloc: test-modreloc-negctl

# --- the two objects the host gate loads --------------------------------------
# Built with $(CFLAGS) -- the KERNEL's own flags, unmodified. That is the whole
# claim: a module is an ordinary kernel translation unit compiled with -c
# instead of being handed to the linker. If this rule needed a special flag,
# the format would not be "a kernel object" and the boot gate below would be
# proving something about a different kind of file.
$(BUILD)/modmod.o: tests/unit/modmod.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/edu_mod.o: c/drivers/core/qemu_edu.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

# The .ko the guest loads. IDENTICAL bytes to edu_mod.o -- the copy exists only
# to give the file the name the boot gate refers to, and the fact that no
# transformation happens between them is the point being demonstrated.
$(MOD_KO): $(BUILD)/edu_mod.o
	@cp $< $@

# --- host gate ----------------------------------------------------------------
# -no-pie is REQUIRED and is not a tidiness flag. The test mmaps the module at
# a fixed LOW address (0x30000000) because that is where the kernel's kmalloc'd
# block is -- identity-mapped physical memory below 512 MiB. The module's
# R_X86_64_PLT32 calls then have to reach the test's own mt_ext/mt_log, and
# under the default PIE those live near 0x55..., about 94 TiB away, which the
# loader would correctly refuse as MOD_E_RANGE. -no-pie puts them at ~0x401000
# and the displacement becomes about -764 MiB, which is the same order as the
# real kernel's.
#
# UBSan is on because this file computes with addresses for a living and the
# one latent bug the gfx line found in a comparable unit was a signed left
# shift nobody could see (CLAUDE.md, "a latent UB"). ASan is NOT on: it
# reserves large parts of the address space and takes MAP_FIXED_NOREPLACE at
# 0x30000000 away, so the test would report a mapping failure instead of a
# result. Tried, watched, and this is why it is absent rather than forgotten.
#
# -fno-sanitize=function IS REQUIRED, and it cost an hour, so it is written
# down. `-fsanitize=undefined` turns on -fsanitize=function, which verifies a
# signature record clang emits EIGHT BYTES BEFORE every function before any
# indirect call through a function pointer. The module is compiled with the
# kernel's flags and has no such record, so the check reads eight bytes below
# the mmap'd block and SEGVs -- reported at the call site in main, with the
# faulting address exactly `block - 8`, which reads like a loader bug placing a
# section too low. It is not: it is the sanitizer refusing to call code it did
# not compile. Removing it costs nothing here, because the property
# -fsanitize=function checks (the callee's type matches the pointer's) is one
# this test establishes by construction -- the signatures are written out in
# the typedefs a dozen lines above the call.
test-modreloc: $(BUILD)/modmod.o $(BUILD)/edu_mod.o tests/unit/modreloc_test.c \
               c/kernel/module/modelf.c c/kernel/module/module.h
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -Wall -Wextra -no-pie -fsanitize=undefined \
	    -fno-sanitize=function -fno-sanitize-recover=undefined \
	    $(HOST_INCDIRS) -o $(BUILD)/modreloc_test \
	    tests/unit/modreloc_test.c c/kernel/module/modelf.c
	@$(BUILD)/modreloc_test $(BUILD)/modmod.o $(BUILD)/edu_mod.o

# --- host negative control ----------------------------------------------------
# -DMODRELOC_NO_RANGE_CHECK is the PLAUSIBLE WRONG LOADER, not a mutilated one:
# it is what somebody writes after reasoning that a 512 MiB machine cannot
# overflow a 32-bit field. Under it, test 12 (a module mapped at 16 GiB) is
# expected to relocate "successfully" with every absolute pointer truncated to
# its low 32 bits, so the positive test MUST fail. Watched failing, with the
# count recorded in the recipe's own message below.
test-modreloc-negctl: $(BUILD)/modmod.o $(BUILD)/edu_mod.o tests/unit/modreloc_test.c \
                      c/kernel/module/modelf.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -no-pie -DMODRELOC_NO_RANGE_CHECK \
	    $(HOST_INCDIRS) -o $(BUILD)/modreloc_negctl \
	    tests/unit/modreloc_test.c c/kernel/module/modelf.c
	@if $(BUILD)/modreloc_negctl $(BUILD)/modmod.o $(BUILD)/edu_mod.o \
	      > $(BUILD)/modreloc_negctl.log 2>&1; then \
	    echo "NEGCTL FAILED: the loader passed with its range checks removed"; \
	    cat $(BUILD)/modreloc_negctl.log; exit 1; \
	 else \
	    echo "negctl ok: $$(grep -c '^FAIL' $(BUILD)/modreloc_negctl.log) assertion(s) redden without the range check"; \
	    grep '^FAIL' $(BUILD)/modreloc_negctl.log || true; \
	 fi

# --- boot gate ----------------------------------------------------------------
# THE CLAIM: the module-built driver behaves IDENTICALLY to the static one.
#
# It is run as a pair of boots of the SAME kernel -- one built without
# c/drivers/core/qemu_edu.c compiled in at all -- against the same QEMU
# command line with `-device edu`:
#
#   without loading   the edu device is present and NOTHING claims it. This is
#                     the negative control and it is not synthetic: it is the
#                     machine the module exists to fix.
#   after loading     the exact `LOGIT_IRQ_OK msi mode=msi ...` and
#                     `LOGIT_IRQ_OK legacy mode=intx ...` lines that
#                     tests/boot/run-devmodel-test.sh requires of the STATIC
#                     driver, produced by the same source compiled with -c.
#
# ci-boot deliberately NOT declared: this target relinks the kernel with a
# different C_SRC and would fight a parallel `make` for build/. Run it alone.
#
# THE CONTROL RUNS FIRST AND BUILDS; THE POSITIVE REUSES ITS BYTES.
# That ordering is not a concession to audit_tests.py's "make it a PREREQUISITE
# of its positive" rule -- it is the stronger arrangement, and it happens to be
# that shape. Two boots are only a controlled pair if they differ in ONE thing,
# and this tree has several agents rebuilding build/ concurrently, so two
# ordinary invocations can silently differ in an unrelated object. So: the
# control builds the kernel (without qemu_edu.c), the module and the disk, and
# boots them WITHOUT loading -- nothing may claim the device. Then the positive
# runs SKIP_BUILD=1 against those exact files, whose sha256 both runs print, and
# loads the module. The only difference between the two boots is four lines
# typed at a shell.
#
# Measured 2026-08-20, both runs on
#   logit.iso 0070622b27d1..  disk.img 5c1f2340f735..  edu.ko be2c2f8ba25b..
#   LOAD=0  no [edu] line, no LOGIT_IRQ_OK line at all
#   LOAD=1  [edu] 0000:00:04.0 live; LOGIT_IRQ_OK msi mode=msi vec=96 count=1
#           payload=2a; LOGIT_IRQ_OK legacy mode=intx vec=96 count=2 payload=2a
test-module: test-module-negctl
	@SKIP_BUILD=1 bash tests/boot/run-module-test.sh
	@# The kernel this pair built has no edu driver in it ON PURPOSE, and make
	@# compares timestamps, not C_SRC -- so leaving it in build/ would hand the
	@# next `make run` a kernel quietly missing a driver. One relink to undo.
	@rm -f $(BUILD)/kernel.elf $(ISO)
	@echo "(removed build/kernel.elf + $(ISO): they are the module-only build)"

test-module-negctl: $(MOD_KO)
	@LOAD=0 bash tests/boot/run-module-test.sh
