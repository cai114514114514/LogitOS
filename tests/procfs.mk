# /proc -- the host gate, its negative control, and the on-device half.
#
# INCLUDED FROM tests/exec.mk, not from the top-level Makefile, for the reason
# tests/poll.mk states in its own header: the Makefile is edited by several
# lines at once and `-include` nests, so hanging this off the fragment that
# already covers c/kernel/exec and c/fs costs nobody a merge. If somebody later
# wants it direct, the one line is `-include tests/procfs.mk` beside the others
# near the end of the Makefile and the include at the bottom of tests/exec.mk
# comes out.
#
# THE KERNEL SIDE NEEDS NO BUILD-SYSTEM CHANGE. C_SRC globs c/fs, so
# c/fs/procfs.c and c/fs/procfs_src.c compile and link by existing. Verified:
# build/c/fs/procfs.o and build/c/fs/procfs_src.o appear in the kernel link
# line.
#
# WHAT THE MAKEFILE DOES NEED, and only for the BOOT gate -- the host gates
# below need nothing. The three consumers are ordinary CLI programs and the
# Makefile lists those by name:
#
#     Makefile:461   CLI := sh echo ls cat pwd wc head ... syslogd ps free uptime
#
# One word each at the end of that list. Nothing else: CLI_RULE builds them and
# the $(DISK) recipe already packs `$(foreach c,$(CLI),$(BUILD)/$(c).aex:/bin/$(c))`.
# Until that line exists, the harness passes CLI on the make command line, which
# is EQUIVALENT here because CLI is a simply-expanded variable used only through
# that foreach and through CLI_AEX -- see the header of run-procfs-test.sh for
# the literal invocation.

.PHONY: test-procfs test-procfs-negctl test-procfs-os

# ci-host: the control is a PREREQUISITE of the positive rather than a sibling
# on the ci-host line. tools/audit_tests.py's NOT_CI drops every
# `test-*-negctl` from what tools/ci.sh runs, on the ground that a control is
# run BY its positive counterpart -- so naming it beside the positive would
# exclude it and then invoke it from nowhere, which is the "stranded control"
# category CLAUDE.md counts 55 of.
ci-host: test-procfs
test-procfs: test-procfs-negctl

PROCFS_SRC := tests/unit/procfs_test.c c/fs/procfs.c
PROCFS_DEP := c/fs/procfs.h c/fs/vfs.h c/fs/vfs_path.h

# -iquote AND NOT -I. c/fs/vfs.h pulls in vfs_meta.h, and this directory list
# would otherwise put mini-libc's own headers on the path for the HOST compiler
# -- the flat-namespace trap CLAUDE.md records twice and tests/poll.mk met from
# this same side. -iquote applies only to "quoted" includes, so the tree's own
# `#include "procfs.h"` resolves and glibc's `#include <stdio.h>` does not go
# looking in c/apps/libc/include.
PROCFS_INC := -iquote c/fs -iquote include/abi
# ASan + UBSan: this file does offset arithmetic into a fixed buffer for a
# living, and every one of its bounds is a candidate for an off-by-one that a
# correctness check cannot see (a render one byte short still parses).
PROCFS_CF  := -O1 -g -Wall -Wextra -fsanitize=address,undefined \
              -fno-sanitize-recover=undefined

$(BUILD)/procfs_test: $(PROCFS_SRC) $(PROCFS_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(PROCFS_CF) $(PROCFS_INC) -o $@ $(PROCFS_SRC)

$(BUILD)/procfs_test_negctl: $(PROCFS_SRC) $(PROCFS_DEP)
	@mkdir -p $(BUILD)
	$(CC) $(PROCFS_CF) -DPROCFS_SNAPSHOT_AT_OPEN $(PROCFS_INC) -o $@ $(PROCFS_SRC)

test-procfs: $(BUILD)/procfs_test
	@$(BUILD)/procfs_test

# THE NEGATIVE CONTROL, and it is the PLAUSIBLE wrong implementation rather
# than a mutilation. pf_size() and the first pf_pread() of a file render the
# same bytes microseconds apart, so sharing one render between them is an
# obvious saving -- and c/kernel/exec/file.c calls vfs_size() at open(). Take
# the saving and every /proc file silently becomes a snapshot taken at open:
# correctly formatted, right length, and wrong in the one way this filesystem
# exists to be right about.
#
# WATCHED FAILING, 2026-08-20: 5 of 73 checks redden and they are exactly the
# five whose names begin "LIVE:" --
#   LIVE: the read reflects the change, not the open
#   LIVE: two reads at offset 0 are two instants
#   LIVE: meminfo re-reads the allocator
#   LIVE: reading a gone process is ENOENT, not the bytes taken at open
#   LIVE: a rewind to 0 re-takes the instant
# The recipe asserts BOTH the count and that every failing line is a LIVE one,
# because "5 things broke" and "the right 5 things broke" are different claims
# and only the second one says the control is aimed at the property.
test-procfs-negctl: $(BUILD)/procfs_test_negctl
	@if $(BUILD)/procfs_test_negctl > $(BUILD)/procfs_negctl.log 2>&1; then \
	    echo "NEGCTL FAILED: /proc passed with its reads served from the open-time render"; \
	    cat $(BUILD)/procfs_negctl.log; exit 1; \
	 else \
	    nf=$$(grep -c '^FAIL ' $(BUILD)/procfs_negctl.log); \
	    nlive=$$(grep '^FAIL ' $(BUILD)/procfs_negctl.log | grep -c 'LIVE:'); \
	    echo "negctl ok: $$nf assertion(s) redden, $$nlive of them liveness checks"; \
	    grep '^FAIL ' $(BUILD)/procfs_negctl.log; \
	    if [ "$$nf" != "5" ] || [ "$$nlive" != "5" ]; then \
	        echo "NEGCTL DRIFT: expected exactly 5 failures, all LIVE:"; exit 1; \
	    fi; \
	 fi

# --- the boot gate ------------------------------------------------------------
# THE CLAIM: /bin/ps -- an ordinary program that opens files -- lists at least
# the console shell and itself, on the real machine, out of a real mount.
#
# ci-boot deliberately NOT declared: it boots QEMU and would fight a parallel
# `make` for build/. Run it alone.
test-procfs-os:
	@bash tests/boot/run-procfs-test.sh
