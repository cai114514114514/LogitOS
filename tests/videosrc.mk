# tests/videosrc.mk -- <video src> direct playback and <track> subtitles.
#
# One of the pre-wired empty fragments (the "-include tests/videosrc.mk" line
# in the root Makefile is the only thing that was here). This file fills it.
#
# WHAT IS BEING GATED, and why each half exists rather than trusting
# test-video-page (tests/mse.mk, which drives the real browser on the machine):
#
#   The ENGINE'S HALF of <video src>. mel_load_bytes + the pump + the clock,
#    over a whole film, on the host with a stepped clock -- reproducible and
#    bisectable, the same reason tests/unit/mse_test.c exists next to
#    test-mse-os.
#
#   SUBTITLES, which test-video-page does not touch at all. A <track> is
#    bytes in, cues at media time, pixels out; the first two are engine
#    arithmetic and are gated HERE, the pixels are gated on the machine by
#    tests/qmp/qmp_videosrc.py (test-videosrc-os), which counts bright pixels
#    in the caption band of an all-black video -- on black content every
#    bright pixel is the caption's, so the count cannot be measuring the
#    video.
#
# The negctl is a PREREQUISITE of both positives, not a sibling: rule 4.

.PHONY: test-videosrc test-videosrc-negctl test-videosrc-os

VSRC_FX := tests/fixtures/video
VSRC_SRC := c/apps/browser/js_media_src.c
VSRC_HDR := c/apps/browser/js_media.h

# The link set is the one tests/mse.mk spent its whole header comment
# justifying (the wildcard caught mjpeg.c, which grew a c/lib/image dependency;
# see the mjpeg note there). Deferred ( = ) for the same reason as there:
# IMG_HOST_SRC / RUST_LIB_HOST are defined by the root Makefile before any
# -include is read, and := would capture an empty value the day the lines
# move. The kmalloc shim is mse.mk's generated file -- generating a second
# copy here would be the hand-copied-source drift AGENTS.md warns about, one
# directory over.
VSRC_IMG   = $(IMG_HOST_SRC) $(RUST_LIB_HOST)
VSRC_INC   = -Ic/apps/browser -Ic/lib/media -Ic/lib/video -Ic/lib/audio \
             $(IMG_HOST_INC)
VSRC_KSHIM = $(BUILD)/mse_kshim.c

$(BUILD)/videosrc_test: tests/unit/videosrc_test.c $(VSRC_SRC) $(VSRC_HDR) \
                        $(VSRC_IMG) $(VSRC_KSHIM) $(wildcard c/lib/media/*.c)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -o $@ \
	    tests/unit/videosrc_test.c $(VSRC_KSHIM) $(VSRC_SRC) \
	    c/lib/media/*.c c/lib/video/*.c c/lib/audio/*.c \
	    $(VSRC_IMG) $(VSRC_INC) -lm

test-videosrc: test-videosrc-negctl $(BUILD)/videosrc_test
	@$(BUILD)/videosrc_test

# --- THE NEGATIVE CONTROL ----------------------------------------------------
# -DSUBS_CONTROL_HIDE silences ONLY mel_subs_active's report: parsing, timing
# and playback all still run. REQUIRED TO FAIL, and required to fail on the
# cue assertions specifically while the playback assertions still pass -- the
# same shape test-mse-nocard-negctl uses, because it is the shape that proves
# the suite notices THIS defect rather than proving the suite is unhappy.
# A control that failed the playback half too would pass on any Tuesday where
# decoding broke, and would have said nothing about subtitles.
test-videosrc-negctl: $(VSRC_SRC) $(VSRC_HDR) $(VSRC_IMG) $(VSRC_KSHIM) \
                      tests/unit/videosrc_test.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DSUBS_CONTROL_HIDE=1 -o $(BUILD)/videosrc_test_neg \
	    tests/unit/videosrc_test.c $(VSRC_KSHIM) $(VSRC_SRC) \
	    c/lib/media/*.c c/lib/video/*.c c/lib/audio/*.c \
	    $(VSRC_IMG) $(VSRC_INC) -lm
	@if $(BUILD)/videosrc_test_neg > $(BUILD)/videosrc_neg.log 2>&1; then \
	    echo "NEGCTL-FAIL: the cue report was silenced and the suite still passed --"; \
	    echo "  which means nothing in it asks the engine what is active."; \
	    exit 1; \
	 elif grep -q 'at t=0.2s the FIRST cue is active' $(BUILD)/videosrc_neg.log && \
	      grep -q 'the element reached' $(BUILD)/videosrc_neg.log; then \
	    echo "negctl: the silenced cue report is caught, and caught where predicted:"; \
	    grep -m1 'FAIL: at t=0.2s' $(BUILD)/videosrc_neg.log | sed 's/^/       /'; \
	    echo "       (playback still passed -- see the reaching-ended line:)"; \
	    grep -m1 'ok: the element reached' $(BUILD)/videosrc_neg.log | sed 's/^/       /'; \
	    grep -c '^FAIL' $(BUILD)/videosrc_neg.log | sed 's/^/       total failures: /'; \
	 else \
	    echo "NEGCTL-FAIL: the sabotaged build failed, but NOT on the cue assertions"; \
	    echo "  -- so this proves the suite is unhappy, not that it gates subtitles."; \
	    grep -m5 '^FAIL' $(BUILD)/videosrc_neg.log | sed 's/^/       /'; exit 1; \
	 fi

# --- on the machine, through the browser ------------------------------------
# The page loads over HTTP in browser.aex with a <video> whose only texture is
# BLACK and a <track default>; the driver screenshots the caption band inside
# a cue's window and inside a gap, and counts bright pixels. Frames-shown is
# read off the element's own counters exactly like test-video-page, because a
# black frame and a stall screenshot identically.
#
# Prerequisite chain, deliberately: the host gate (and through it the negctl)
# runs FIRST -- a failure there names the defect in a process you can run
# under a debugger, and the 90-second QEMU boot is not spent discovering what
# a host gate already knew.
test-videosrc-os: test-videosrc $(ISO) $(DISK)
	@python3 tests/qmp/qmp_videosrc.py $(ISO) $(DISK)

ci-host: test-videosrc
ci-boot: test-videosrc-os
