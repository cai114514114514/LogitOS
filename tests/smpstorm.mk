# --- The multi-core fork/exec wedge -----------------------------------------
#
# docs/superpowers/specs/2026-06-08-smp-bkl-deadlock.md records a -smp 4
# g_bkl <-> g_sched_lock ABBA deadlock and marks it FIXED. Something in that
# class is BACK, and this is the smallest thing that shows it.
#
# WHAT IS MEASURED, not asserted: a long run of ordinary `fork + execve + exit`
# from /bin/sh. The program forked is deliberately the dullest one in the
# tree -- /bin/libctest has no AetherScript, no GUI, no network and no
# filesystem writes -- so a wedge here cannot be blamed on any subsystem that
# happened to be under change when it turned up.
#
# MEASURED ON 2026-08-16, same ISO, 120 reps:
#
#     -smp 4   wedged at rep 78 | PASS | wedged at rep 70
#     -smp 1   PASS
#     -smp 4, 40 reps   PASS
#
# So it is core-count dependent (single core never wedges at the same load) and
# the wedge POINT MOVES, which rules out a fixed limit being hit and says race.
# Every wedged run printed `sh: cannot open output` -- a failed open in the
# shell's redirect path -- in the rep immediately before the machine stopped,
# which is the thread to pull.
#
# test-smp-fork-storm is therefore EXPECTED TO BE FLAKY and is not wired into
# `make test`. It is a reproducer, not a gate; the number it prints is the one
# to bisect on. test-smp-fork-storm-1core is its control: the same load on one
# core must pass, and if it ever stops passing this stops being an SMP bug.
.PHONY: test-smp-fork-storm test-smp-fork-storm-1core

test-smp-fork-storm: $(ISO) $(DISK)
	@bash tests/boot/run-smp-fork-storm.sh $(ISO) $(DISK) $(STORM_REPS) 4

test-smp-fork-storm-1core: $(ISO) $(DISK)
	@bash tests/boot/run-smp-fork-storm.sh $(ISO) $(DISK) $(STORM_REPS) 1

STORM_REPS ?= 120
