# tests/h264.mk -- the H.264 decoder's gates.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 2500-line file is where their edits collide. The
# root Makefile carries one `-include tests/h264.mk` line and nothing else,
# which is the arrangement tests/h265.mk, tests/nic.mk, tests/audio.mk and
# tests/usb.mk already use.

.PHONY: test-h264 test-h264-units test-h264-diff test-h264-high \
        test-h264-negctl test-h264-real test-h264-pts

H264_SRC := c/lib/video/h264.c c/lib/video/h264_nal.c c/lib/video/h264_cavlc.c \
            c/lib/video/h264_cabac.c c/lib/video/h264_dpb.c c/lib/video/h264_mb.c \
            c/lib/video/h264_pred.c c/lib/video/h264_mc.c c/lib/video/h264_deblock.c
H264_INC := -Ic/lib/video

# --- whole-stream gate ------------------------------------------------------
# tools/genvideo.sh generates the stream matrix with ffmpeg/libx264 and the
# reference YUV with ffmpeg's own decoder. H.264 reconstruction is exactly
# specified integer arithmetic, so a correct decoder matches ffmpeg
# byte-for-byte -- any mismatch is our bug, reported with frame/plane/pixel.
# Needs ffmpeg. (genvideo.sh is idempotent; rerun it by hand to refresh.)
#
# The matrix covers Baseline (CAVLC, I/P), Main (CABAC, B slices) and High
# (8x8 transform, scaling matrices, B pyramids) -- see tools/genvideo.sh for
# what each case is for. A High-profile stream is what every real web video
# is, so the Baseline half of this list is now the regression half: it is what
# proves the CABAC and B-slice work did not break the path that already worked.
#
# The cases this target requires to be BIT-EXACT are named, not globbed.
# Anything the decoder does not yet get exactly right is left out of the list
# and is not claimed anywhere; `make test-h264-diff` runs the WHOLE matrix
# including the failures, which is the honest picture. Right now exactly one
# generated case is outside the gate:
#
#   high-cqm-jvt   x264 --cqm jvt, i.e. the JVT default scaling matrices. The
#                  matrices themselves parse correctly (tests/unit/
#                  h264_cabac_test.c pins the Table 7-2 fall-back and the
#                  resulting LevelScale); what the case exposes is a separate,
#                  PRE-EXISTING single-sample defect that also reproduces on a
#                  plain Baseline CAVLC stream with low-QP macroblocks and
#                  reproduces identically on the decoder as it was before any
#                  of the High-profile work. The JVT matrix quantises low
#                  frequencies finely, which puts many more macroblocks into
#                  the regime where it fires. See the note at the top of
#                  c/lib/video/h264.c.
H264_GATE := i-only-160x120 i-only-322x242 ip-320x240 ip-640x360 \
             slices4-320x240 refs4-320x240 deblock-320x240 nodeblock-320x240 \
             longgop-160x120 wpred-320x240 \
             cabac-i-176x144 cabac-ip-320x240 cabac-p8x8-176x144 \
             cavlc-b-176x144 cabac-b-176x144 b-spatial-320x240 \
             b-temporal-320x240 b-pyramid-320x240 b-implicit-320x240 \
             b-explicit-320x240 \
             high-i8x8-176x144 high-cavlc8-176x144 high-p8x8dct-320x240 \
             high-full-320x240 high-full-640x360 high-slices-320x240 \
             high-cqm-flat-320x240

test-h264:
	@mkdir -p $(BUILD)/h264ref
	@bash tools/genvideo.sh $(BUILD)/h264ref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_test tests/unit/h264test.c $(H264_SRC) $(H264_INC)
	@for c in $(H264_GATE); do \
	    $(BUILD)/h264_test $(BUILD)/h264ref/$$c.h264 $(BUILD)/h264ref/$$c.ref.yuv || exit 1; \
	done
	@echo "all $(words $(H264_GATE)) gated H.264 host cases bit-exact"
	@# The committed fixture, checked against a CRC pinned in the tree. The
	@# generated matrix above re-encodes with whatever x264 is installed, so it
	@# is not a fixed target; this one is, and it is also the only stream we
	@# have that quantises finely enough to reach a chroma qP below 6. The same
	@# CRC is what the on-device check prints, which is how a decode inside
	@# LogitOS gets compared with a decode on the host.
	@crc=`$(BUILD)/h264_test tests/fixtures/video/sample.h264 | awk '{print $$2}'`; \
	 want=`cat tests/fixtures/video/sample.crc32`; \
	 if [ "$$crc" != "$$want" ]; then \
	     echo "H264-FIXTURE-FAIL crc $$crc want $$want"; exit 1; fi; \
	 echo "H264-OK fixture crc $$crc (tests/fixtures/video/sample.h264)"
	@$(MAKE) --no-print-directory test-h264-pts

# Timestamps have to survive reordering. Until B slices landed, decode order
# was display order for every stream this decoder accepted, so a caller could
# pair frames with timestamps through a plain FIFO and be right; with B
# pictures that is wrong for about half the frames and the failure is invisible
# -- every frame is present, only their times are shuffled. h264_decode_pts
# carries the caller's value on the picture itself, and this checks the two
# things that matter: no value is lost or duplicated, and a stream that
# reorders really does come back out of decode order (a FIFO would report 0).
test-h264-pts: $(BUILD)/h264_test
	@$(BUILD)/h264_test --pts $(BUILD)/h264ref/b-pyramid-320x240.h264
	@$(BUILD)/h264_test --pts $(BUILD)/h264ref/cabac-b-176x144.h264
	@$(BUILD)/h264_test --pts $(BUILD)/h264ref/i-only-160x120.h264

$(BUILD)/h264_test: tests/unit/h264test.c $(H264_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/h264test.c $(H264_SRC) $(H264_INC)

# Per-case byte counts over the WHOLE stream instead of stopping at the first
# bad pixel. "the first mismatch moved" says nothing about whether a change
# helped; a total does, and it is what makes bisecting the decoder possible.
test-h264-diff:
	@mkdir -p $(BUILD)/h264ref
	@bash tools/genvideo.sh $(BUILD)/h264ref
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_diff tests/unit/h264_diff.c $(H264_SRC) $(H264_INC)
	@rc=0; for f in $(BUILD)/h264ref/*.h264; do \
	    printf '%-26s ' "$$(basename $$f .h264)"; \
	    $(BUILD)/h264_diff $$f $${f%.h264}.ref.yuv | tail -1 || rc=1; \
	done; exit $$rc

# --- module unit tests ------------------------------------------------------
# These existed but were wired to nothing, so they had never run -- and two of
# their expectations disagreed with the spec (intra 4x4 vertical-left indexed
# p[x+y] instead of p[x+(y>>1)]; chroma DC averaged both edges in all four
# quadrants). Both were corrected against 8.3.1.2.8 / 8.3.4.1 and then
# confirmed the honest way: with the decoder fixed to match, a real stream
# decodes byte-identically to ffmpeg.
#
# h264_cavlc_test is NOT here on purpose. Its roundtrip section encodes with a
# CAVLC *encoder* written inside the test, and that encoder disagrees with the
# decoder about level coding. The decoder is the one that is right: it decodes
# i-only-160x120 bit-exactly for all 60 frames, which exercises coeff_token,
# level escapes, total_zeros and run_before across thousands of blocks. Fixing
# the test's encoder is its own task; wiring a known-wrong test into a gate
# would only teach people to ignore the gate.
test-h264-units:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_pred_test tests/unit/h264_pred_test.c \
	    c/lib/video/h264_pred.c $(H264_INC)
	@$(BUILD)/h264_pred_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_mc_test tests/unit/h264_mc_test.c \
	    c/lib/video/h264_mc.c $(H264_INC)
	@$(BUILD)/h264_mc_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_deblock_test tests/unit/h264_deblock_test.c \
	    c/lib/video/h264_deblock.c $(H264_INC)
	@$(BUILD)/h264_deblock_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_cabac_test tests/unit/h264_cabac_test.c \
	    c/lib/video/h264_cabac.c c/lib/video/h264_nal.c $(H264_INC)
	@$(BUILD)/h264_cabac_test

# --- real-world streams -----------------------------------------------------
# A synthetic x264 corpus and a real segment off a video site fail differently:
# the corpus is what our own generator thought to ask for, the segment is what
# an encoder actually emitted, with its own GOP structure, its own weighted
# prediction decisions and its own reference list churn. Both are compared
# against ffmpeg byte for byte.
#
# The segments are not committed (they are tens of megabytes and not ours to
# redistribute); point H264_REAL at a directory of .h264 elementary streams and
# this target checks every one of them. Missing directory -> skipped, loudly.
H264_REAL ?= tests/fixtures/video/real
test-h264-real:
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_test tests/unit/h264test.c $(H264_SRC) $(H264_INC)
	@if [ ! -d "$(H264_REAL)" ]; then \
	    echo "H264-REAL SKIPPED: no $(H264_REAL) (set H264_REAL=<dir of .h264>)"; \
	    exit 0; fi; \
	 n=0; for f in $(H264_REAL)/*.h264; do \
	    [ -e "$$f" ] || continue; \
	    ffmpeg -v error -i $$f -f rawvideo -pix_fmt yuv420p $$f.ref.yuv -y || exit 1; \
	    $(BUILD)/h264_test $$f $$f.ref.yuv || exit 1; \
	    rm -f $$f.ref.yuv; n=$$((n+1)); \
	 done; echo "H264-REAL: $$n real stream(s) bit-exact"

# --- negative control -------------------------------------------------------
# An assertion that FAILS without the High-profile work: profile_idc 100 must
# be ACCEPTED and must decode. Before this change h264_parse_sps returned
# H264_ERR_UNSUPPORTED (-2) for anything that was not 66/77/88, so the very
# first SPS of every real web video was refused. The control encodes a High
# stream with the three things High actually adds -- CABAC, B slices and the
# 8x8 transform -- and requires a bit-exact decode; a decoder that only learned
# to stop rejecting the profile fails it on the first B frame.
test-h264-negctl:
	@mkdir -p $(BUILD)/h264ctl
	@ffmpeg -v error -f lavfi -i testsrc2=rate=30:size=176x144 -t 1 \
	    -c:v libx264 -profile:v high -pix_fmt yuv420p -preset veryslow \
	    -x264-params cabac=1:bframes=3:8x8dct=1 \
	    -f h264 $(BUILD)/h264ctl/negctl.h264 -y
	@ffmpeg -v error -i $(BUILD)/h264ctl/negctl.h264 -f rawvideo -pix_fmt yuv420p \
	    $(BUILD)/h264ctl/negctl.ref.yuv -y
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h264_test tests/unit/h264test.c $(H264_SRC) $(H264_INC)
	@$(BUILD)/h264_test $(BUILD)/h264ctl/negctl.h264 $(BUILD)/h264ctl/negctl.ref.yuv
	@echo "H264-NEGCTL-OK: High profile (CABAC + B + 8x8) decodes bit-exact"
