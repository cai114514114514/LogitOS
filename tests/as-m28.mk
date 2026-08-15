# tests/as-m28.mk -- gates for M28: capabilities, bounded regions, slices.
#
# Its own fragment rather than more lines in the Makefile, for the reason
# tests/libc.mk, tests/audio.mk and tests/h265.mk are: several lines edit the
# root Makefile concurrently and a shared-file edit cannot be committed without
# swallowing theirs. The root Makefile pulls this in with one line.
#
# WHY A SECOND HOST BATTERY AND NOT MORE CASES IN as_test.c. Half of what M28
# has to prove is deliberately unreachable from a script: as_caps_set() and
# as_cap_attenuate() have no script-visible entry point, because a capability a
# script can construct is not a capability. tests/unit/as_cap_test.c drives that
# half from C, against the same objects the VM uses, and drives the other half
# (bounded regions, slices, the CAP_RAW gate, the port constructors) through
# ordinary snippets. as_test.c stays what it is -- run a snippet, assert its
# output.
#
# THE TWO NEGATIVE CONTROLS ARE THE POINT OF THIS FILE. Before as_cap_test.c
# existed, both -D flags were vacuous: the tree compiled with every capability
# check removed and all 350 as_test checks still passed, because nothing in that
# battery ever indexed a region out of range or opened a port without a
# capability. A control nobody has watched fail is not evidence.
#
# Watched failing on 2026-08-14, which is the only claim that makes them mean
# anything:
#   -DAS_REGION_NO_BOUNDS -> 7/4152 fail (oob get/set high and low, slice past
#      the end, reversed slice, and oob-is-catchable, which stops raising at all)
#   -DAS_CAP_NO_CHECK     -> 8/4148 fail (addr/alloc/syscall ungated, and the
#      four port constructors opening /dev/null and forking without a grant)
#
# NEITHER CONTROL IS ALLOWED TO FAIL BY CRASHING. Every assertion the controls
# break is chosen to be harmless when its check is removed -- addr() returns an
# integer, alloc() mallocs, /dev/null opens -- and the assertions that would
# dereference a bad address ungated (peek8(0), poke8(0,0), mem2str(0,1)) are
# compiled out of the control builds with #ifndef AS_CAP_NO_CHECK. A SIGSEGV is
# a nonzero exit and would make these targets "succeed", but it would prove the
# process died rather than that an assertion noticed, which is the distinction
# the whole discipline exists to keep.

.PHONY: test-as-cap test-as-cap-negctl test-as-region-negctl test-as-m28 test-as-cap-os

AS_CAP_SRC := tests/unit/as_cap_test.c

# The positive gate. Attenuation is checked over the WHOLE six-bit lattice --
# 64 x 64 ordered (held, requested) pairs -- rather than a handful of hand-picked
# ones, so there is no interesting case left to have overlooked, plus the prefix
# boundary cases where a substring test would let scope("/usr") reach "/usrx".
test-as-cap: check-asops check-abi
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/as_cap_test $(AS_CAP_SRC) $(AS_CORE) \
	    -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_cap_test
	@$(CC) -O2 -w -DAS_GC_STRESS -o $(BUILD)/as_cap_gc $(AS_CAP_SRC) $(AS_CORE) \
	    -Ic/apps/as -Iinclude/abi
	@$(BUILD)/as_cap_gc
	@echo "  (and again under -DAS_GC_STRESS -- a slice holds its parent alive)"

# A slice is a VIEW, so the collector must not free the buffer it points into
# while the slice is live. That is the one M28 object relationship GC stress can
# break, and it is why the positive gate runs twice.

define AS_M28_NEGCTL
test-as-$(1)-negctl: check-asops check-abi
	@mkdir -p $$(BUILD)
	@$$(CC) -O2 -w -D$(2) -o $$(BUILD)/as_cap_$(1)_negctl $$(AS_CAP_SRC) $$(AS_CORE) \
	    -Ic/apps/as -Iinclude/abi
	@if $$(BUILD)/as_cap_$(1)_negctl > $$(BUILD)/as_cap_$(1).log 2>&1; then \
	    echo "FAIL: the M28 battery passed with -D$(2) -- $(3) is not load-bearing"; \
	    exit 1; \
	 elif [ $$$$? -gt 1 ]; then \
	    echo "FAIL: -D$(2) CRASHED the battery (exit $$$$?) instead of failing an assertion."; \
	    echo "      A dead process is not evidence that a check was missed; see this file's header."; \
	    tail -3 $$(BUILD)/as_cap_$(1).log; exit 1; \
	 else \
	    echo "negative control ok: without $(3) the battery fails --"; \
	    grep -a "^FAIL" $$(BUILD)/as_cap_$(1).log | head -8; \
	    tail -1 $$(BUILD)/as_cap_$(1).log; \
	 fi
endef

$(eval $(call AS_M28_NEGCTL,region,AS_REGION_NO_BOUNDS,the region bounds check))
$(eval $(call AS_M28_NEGCTL,cap,AS_CAP_NO_CHECK,the capability check))

# --- on target: a script denied CAP_FS provably cannot read /etc --------------
#
# WRITTEN, AND IT CANNOT BE RUN TODAY. The tree cannot build an ISO:
# c/kernel/mm/fault.c calls mm_fault_classify() with an eighth argument and a
# MM_FAULT_FILE class that mm.h does not declare -- uncommitted, half-finished
# work belonging to the page-cache line, and (verified with make -k) the only
# file in the whole kernel that fails to compile. Every other M28 claim in this
# file is host-side and stands on its own; this one is the milestone's headline
# gate and is NOT claimed as passing.
#
# It is here rather than in a branch because the harness is the deliverable: the
# moment the kernel builds, this runs, and nobody has to reconstruct what the
# on-device assertion was supposed to be.
test-as-cap-os: $(ISO) $(DISK)
	@bash tests/boot/run-as-cap-test.sh $(ISO) $(DISK)

# Everything M28 can currently prove, in one target.
test-as-m28: test-as-cap test-as-region-negctl test-as-cap-negctl
	@echo "PASS: M28 -- capabilities, bounded regions and slices, with both controls watched failing"
