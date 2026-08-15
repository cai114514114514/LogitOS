# The desktop's motion gates -- Expose, and the dock fly.
#
# NOT YET WIRED INTO THE MAKEFILE, and deliberately so: this landed while three
# other lines were editing Makefile concurrently, and one `-include` line is a
# cheaper merge for whoever owns it than a conflict is for everybody. To
# register it, add next to the other `-include tests/*.mk` lines near the bottom
# of the Makefile:
#
#     -include tests/motion.mk
#
# Until then the gate runs directly, and its header documents that:
#
#     python3 tests/qmp/qmp_motion.py
#     python3 tests/qmp/qmp_motion.py --negative
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
test-motion: $(ISO) $(DISK)
	@mkdir -p $(MOTION_OUT)
	python3 tests/qmp/qmp_motion.py --iso $(ISO) --disk $(DISK) --out $(MOTION_OUT)

# Builds a kernel that reports only where a moving window is GOING (wm.c's
# WM_ANIM_DAMAGE_LIE) in a throwaway copy of the tree, and SUCCEEDS only when
# the pixel checks above fail against it.
test-motion-negctl:
	@mkdir -p $(MOTION_OUT)-neg
	python3 tests/qmp/qmp_motion.py --disk $(DISK) --out $(MOTION_OUT)-neg --negative
