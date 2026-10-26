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

# RETIRED WITH THE A2 ENGINE (2026-09-16), and missed by the change that
# tombstoned test-as and its siblings in the Makefile: the host battery above --
# test-as-cap (run twice, the second time under -DAS_GC_STRESS), its two
# controls test-as-region-negctl and test-as-cap-negctl, and the test-as-m28
# umbrella over the three. tests/unit/as_cap_test.c #included legacy/vm.h and
# drove as_caps_set()/as_cap_attenuate() on the A2 VM's own objects; both died
# with the VM, so from that day every one of these targets failed at #include
# before a single assertion ran -- reported by a host sweep on the macOS dev
# host, 2026-10-26. The 2026-08-14 watched-failing numbers above stay as the
# record of what the battery once proved.
#
# The capability claim itself lives on where A3 can answer it: test-as-cap-os
# below cross-compiles the original example and checks three real kernel grants
# in each build mode. A3 has no in-process attenuation API for a C battery to
# drive, so the lattice sweep is not rebuilt speculatively here.
# tests/unit/as_cap_test.c was deleted in the same change -- a red gate that
# cannot compile is not coverage.

.PHONY: test-as-cap-os

# --- on target: a script denied CAP_FS provably cannot read /etc --------------
#
# WRITTEN, AND IT CANNOT BE RUN TODAY. The tree cannot build an ISO:
# c/kernel/mm/virt/fault.c calls mm_fault_classify() with an eighth argument and a
# MM_FAULT_FILE class that mm.h does not declare -- uncommitted, half-finished
# work belonging to the page-cache line, and (verified with make -k) the only
# file in the whole kernel that fails to compile. Every other M28 claim in this
# file is host-side and stands on its own; this one is the milestone's headline
# gate and is NOT claimed as passing.
#
# It is here rather than in a branch because the harness is the deliverable: the
# moment the kernel builds, this runs, and nobody has to reconstruct what the
# on-device assertion was supposed to be.
# A3 correction: the normal gate cross-compiles the original example and
# checks three real kernel grants in each build mode. The standalone shell
# entry still accepts an existing ISO/disk and validates its installed AEX.
test-as-cap-os: test-as-capcheck-guest
