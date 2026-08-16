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
# MEASURED ON 2026-08-16, same ISO, 120 reps, /bin/libctest:
#
#     -smp 4   wedged at rep 13 | wedged at rep 14
#     -smp 1   PASS
#
# So it is core-count dependent: one core never wedges under the same load,
# which rules out a leak that would hit the same count either way.
#
# THE FIRST VERSION OF THIS HARNESS MEASURED SOMETHING ELSE, and the correction
# is worth keeping because it is the more useful of the two facts. It ran
# `$(PROG) > /dev/null`, and this machine HAS NO /dev/null -- /dev holds eight
# synthetic nodes and none of them is null, and it is not a real directory on
# logitfs either, so the VFS falls back to a default mode of 0644, which has no
# execute bit, and even root cannot create in it. The shell therefore refused
# the redirect and the child exited BEFORE execve on every single rep. It still
# forked, so the machine still wedged -- at reps 70 and 78 rather than 13 and
# 14 -- and a harness that never ran the program it names produced a plausible
# number. The `sh: cannot open output` line that appeared just before each
# wedge, and which the first commit reported as a precursor, is not one: it
# happens on every rep from the first.
#
# The kernel now says which gate refused an open (file.c open_refused, and the
# weak vfs_note_refusal hook that reports the credential the decision actually
# used rather than one looked up afterwards). That pair is what turned this
# from a theory into a fact, in one boot.

.PHONY: test-smp-fork-storm test-smp-fork-storm-1core

test-smp-fork-storm: $(ISO) $(DISK)
	@bash tests/boot/run-smp-fork-storm.sh $(ISO) $(DISK) $(STORM_REPS) 4

test-smp-fork-storm-1core: $(ISO) $(DISK)
	@bash tests/boot/run-smp-fork-storm.sh $(ISO) $(DISK) $(STORM_REPS) 1

STORM_REPS ?= 120
