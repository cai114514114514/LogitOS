# The desktop's motion gates -- Expose, and the dock fly.
#
# [CORRECTION KEPT BESIDE THE OLD CLAIM, 2026-08-30] The header below said
# "NOT YET WIRED INTO THE MAKEFILE ... add `-include tests/motion.mk`". That
# was true when written and is not now: the Makefile has carried that exact
# line (beside tests/menu.mk) for some time, while this comment still told
# every reader the gate was unreachable -- stale in the direction that costs
# nothing and explains nothing.
#
# WHAT IT MEASURES, in one line each -- the long version is the docstring at the
# top of tests/qmp/qmp_motion.py:
#   * the geometry of every animated frame, off the guest's own serial trace,
#     asserted to lie strictly between a window's frame and its Expose cell;
#   * three photographs of each flight (start, mid, settled), with the
#     mid-flight one CHOSEN by measuring which shot is least like both ends --
#     because a burst fired at the gesture's announcement lands inside the first
#     (expensive) composite and photographs the state before it;
#   * that a partially composited desktop, after a flight, is pixel-identical to
#     a full composite of the same state.
#
# THE NEGATIVE CONTROL is the last of those and only the last of those. The
# round-trip checks are healed by the whole-screen repaint every gesture ends
# with, so they pass against a lying kernel and prove nothing about per-frame
# damage; `--negative` prints how many of the pixel checks the lie got past, so
# that stays visible rather than being assumed away.

MOTION_OUT ?= build/motion

.PHONY: test-motion test-motion-negctl
# THE CONTROL IS A PREREQUISITE OF ITS POSITIVE, not a line on a ci- aggregate:
# tools/audit_tests.py's NOT_CI drops every `test-*-negctl` from what CI runs
# on the assumption the positive runs it, and naming one on a ci- line
# satisfies the stranded audit while running it never. This control sat
# stranded in tests/audit-stranded.baseline from the day it landed.
test-motion: test-motion-negctl $(ISO) $(DISK)
	@mkdir -p $(MOTION_OUT)
	python3 tests/qmp/qmp_motion.py --iso $(ISO) --disk $(DISK) --out $(MOTION_OUT)

# Builds a kernel that reports only where a moving window is GOING (wm.c's
# WM_ANIM_DAMAGE_LIE) in a throwaway copy of the tree, and SUCCEEDS only when
# the pixel checks above fail against it.
test-motion-negctl:
	@mkdir -p $(MOTION_OUT)-neg
	python3 tests/qmp/qmp_motion.py --disk $(DISK) --out $(MOTION_OUT)-neg --negative
