# ===================== how far is the clock from optimal? =====================
#
# c/kernel/mm/reclaim.c picks its victim with a clock. reclaim.h defends that
# choice against an active/inactive LRU and the defence is sound, but it is an
# argument about mechanism, not a measurement of quality: nothing in this tree
# says how many of the page faults this machine takes were AVOIDABLE.
#
# That number is knowable. Page replacement has an exact offline optimum --
# Belady's MIN, evict the page whose next reference is furthest in the future --
# which cannot be implemented online but CAN be computed on a recorded trace.
# These targets record the trace and compute the gap.
#
#   make mmtrace-tools      build the QEMU plugin + the simulator
#   make test-mmsim         THE CONTROLS. Run this before believing any number
#                           below; see tools/mmtrace/mmsim.c.
#   make mmtrace-video      record /bin/vidcheck decoding H.264
#   make mmtrace-mempress   record the swap test's own pressure program
#   make mmtrace-churn      record fork/exec churn through the shell
#   make mmtrace-report     simulate every trace that exists and print the gap
#
# Its own fragment, like every other tests/*.mk here: several lines share this
# tree and a whole-file Makefile edit from a concurrent line cannot delete a
# file it does not open.
#
# NOTHING HERE CHANGES THE KERNEL. The tracer is a QEMU TCG plugin, so the ISO
# these targets boot is the identical ISO every other harness boots, and a run
# without `-plugin` is byte-for-byte the run it always was. That was a
# requirement, not a convenience: eleven harnesses boot this image.

MMT_DIR   := $(BUILD)/mmtrace
MMT_SO    := $(MMT_DIR)/mmtrace.so
MMT_SIM   := $(MMT_DIR)/mmsim
MMT_SRC   := tools/mmtrace
# libglib2.0-dev is not installed on every machine that can build QEMU plugins,
# and the plugin header needs exactly two of its struct definitions. Use the
# real headers when they are there and a three-type stand-in when they are not;
# see tools/mmtrace/stub/glib.h for why that is safe and where it stops.
MMT_GLIB  := $(shell test -f /usr/include/glib-2.0/glib.h \
               && echo '-I/usr/include/glib-2.0 -I/usr/lib/x86_64-linux-gnu/glib-2.0/include' \
               || echo '-I$(MMT_SRC)/stub')

.PHONY: mmtrace-tools test-mmsim mmtrace-video mmtrace-mempress mmtrace-churn \
        mmtrace-as mmtrace-report mmtrace-clean

$(MMT_SO): $(MMT_SRC)/mmtrace.c $(MMT_SRC)/mmtrace_fmt.h $(MMT_SRC)/qemu-plugin.h
	@mkdir -p $(MMT_DIR)
	@gcc -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden \
	    -I$(MMT_SRC) $(MMT_GLIB) -o $@ $(MMT_SRC)/mmtrace.c

$(MMT_SIM): $(MMT_SRC)/mmsim.c $(MMT_SRC)/mmtrace_fmt.h
	@mkdir -p $(MMT_DIR)
	@gcc -O2 -Wall -Wextra -I$(MMT_SRC) -o $@ $(MMT_SRC)/mmsim.c

mmtrace-tools: $(MMT_SO) $(MMT_SIM)

# --- the controls -------------------------------------------------------------
# THE ONE TO RUN FIRST. A sequential scan of N+1 pages through N frames is the
# worst case for LRU, FIFO and clock alike -- each evicts precisely the page
# needed next, so they miss on EVERY reference while MIN misses about once per
# N. A simulator that does not reproduce that gap has a broken MIN, and every
# other number it prints is worthless.
#
# The mirror control is uniform random with memory far smaller than the page
# set: there every policy including MIN converges, so a harness that shows the
# clock losing there is measuring something other than policy.
#
# Both are asserted, not eyeballed.
test-mmsim: $(MMT_SIM)
	@bash tools/mmtrace/controls.sh $(MMT_SIM)

# --- recording ----------------------------------------------------------------
mmtrace-video: $(ISO) $(DISK) mmtrace-tools
	@bash tests/boot/run-mmtrace.sh $(ISO) $(DISK) video $(MMT_DIR)/video.mmt

mmtrace-mempress: $(ISO) $(DISK) mmtrace-tools
	@bash tests/boot/run-mmtrace.sh $(ISO) $(DISK) mempress $(MMT_DIR)/mempress.mmt

mmtrace-churn: $(ISO) $(DISK) mmtrace-tools
	@bash tests/boot/run-mmtrace.sh $(ISO) $(DISK) churn $(MMT_DIR)/churn.mmt

mmtrace-as: $(ISO) $(DISK) mmtrace-tools
	@bash tests/boot/run-mmtrace.sh $(ISO) $(DISK) as $(MMT_DIR)/as.mmt

# The browser needs no harness of its own: tests/qmp/qmp_site.py launches
# "${QEMU:-qemu-system-x86_64}", so pointing QEMU at the wrapper traces a real
# site load with no edit to that file at all.
#   MMTRACE_OUT=build/mmtrace/kimi.mmt QEMU=tools/mmtrace/qemu-mmtrace \
#       make scoreboard-1 SITE=kimi

# --- the answer ---------------------------------------------------------------
# Frame counts are given as a fraction of each trace's own footprint by
# tools/mmtrace/report.sh, because "128 frames" means something different for a
# 3 MiB workload and a 90 MiB one, and the only regime where a replacement
# policy matters at all is the one where memory is somewhat smaller than the
# working set.
mmtrace-report: $(MMT_SIM)
	@bash tools/mmtrace/report.sh $(MMT_SIM) $(MMT_DIR)

mmtrace-clean:
	@rm -rf $(MMT_DIR)
