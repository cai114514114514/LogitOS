# VP9 -- the key-frame decoder, and an EMPTY exact list that says so.
#
# The shape is H.265's, deliberately: test-vp9 is the bit-exact list and
# anything not exact is NOT in it and is claimed nowhere; test-vp9-diff is the
# whole matrix including the failures, which is the honest picture and the
# thing to bisect with. VP9 reconstruction is exactly specified integer
# arithmetic, so a tolerance here would be a decision to stop measuring.
#
# VP9_GATE IS EMPTY, AND THAT IS THE MEASUREMENT, not an oversight. First run
# of tests/unit/vp9_test.c against a generated corpus, 2026-08-25:
#
#     17 of 17 cases decode every frame and every one of them is WRONG.
#     wrong bytes, worst to best:
#       ragged-352x288   143,332      lossless-160x120     1,082
#       seg-aq1-320x240   94,007      lossless-66x66       1,258
#       seg-aq3-320x240   88,868      tiles2-1280x128      2,189
#       sharp7-320x240    57,943      tilerows2-1280x128   2,189
#       mid-320x240       41,311      ragged-66x66         2,343
#
# THE ROW TO BISECT WITH IS lossless-160x120, and not because it is smallest.
# In VP9's lossless mode every transform is the 4x4 WHT, the DCT is not used
# at all, and the loop filter is OFF -- so 1,082 wrong bytes there rules out
# the two largest surfaces in the decoder and points at reconstruction or
# prediction. A decoder that is wrong in lossless is not wrong by rounding.
#
# Note tiles2-1280x128 and tilerows2-1280x128 report the IDENTICAL first
# mismatch and the IDENTICAL byte count. Two different tile configurations
# failing identically says the tile code is not what is failing there.
#
# The corpus is GENERATED, never committed: tools/genvp9.sh encodes with
# ffmpeg's libvpx-vp9 and decodes the reference with ffmpeg's OWN native vp9
# decoder, on the same machine in the same run. So the pair is self-consistent
# whatever libvpx version is installed -- which is what makes this comparable
# across machines without pinning an encoder.

VP9_INC := -Ic/lib/video
VP9_SRC := c/lib/video/vp9.c c/lib/video/vp9_bool.c c/lib/video/vp9_hdr.c \
           c/lib/video/vp9_idct.c c/lib/video/vp9_lf.c c/lib/video/vp9_pred.c \
           c/lib/video/vp9_token.c

# Nothing is bit-exact yet. When a case becomes exact it is added here, and
# from that moment a regression in it fails the build.
VP9_GATE :=

# NOTE the include path: -Ic/lib/video ONLY. Adding $(INCDIRS) or
# -Ic/apps/libc/include breaks a HOST gcc build, because mini-libc's features.h
# shadows glibc's and __GLIBC_USE(X) then parses as a call. That is the header
# basename collision CLAUDE.md documents for the freestanding build, and it
# bites host builds too -- it cost a compile here before it was recognised.
$(BUILD)/vp9_test: tests/unit/vp9_test.c $(VP9_SRC) c/lib/video/vp9.h \
                   c/lib/video/vp9_int.h c/lib/video/vp9_tables.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(VP9_INC) -o $@ tests/unit/vp9_test.c $(VP9_SRC)

.PHONY: test-vp9 test-vp9-diff
test-vp9: $(BUILD)/vp9_test
	@mkdir -p $(BUILD)/vp9corpus
	@rc=0; bash tools/genvp9.sh $(BUILD)/vp9corpus >/dev/null 2>&1 || rc=$$?; \
	 if [ $$rc != 0 ]; then \
	    echo "VP9: no ffmpeg with libvpx-vp9 -- corpus not generated, nothing measured"; \
	    exit 0; \
	 fi; \
	 if [ -z "$(VP9_GATE)" ]; then \
	    echo "VP9: the bit-exact list is EMPTY -- no case decodes exactly yet."; \
	    echo "     This target passes because it claims nothing. Run test-vp9-diff"; \
	    echo "     for the picture, and read the header of tests/vp9.mk first."; \
	    exit 0; \
	 fi; \
	 for c in $(VP9_GATE); do \
	    $(BUILD)/vp9_test $(BUILD)/vp9corpus/$$c.ivf \
	        $(BUILD)/vp9corpus/$$c.ref.yuv || exit 1; \
	 done; \
	 echo "VP9-OK $(words $(VP9_GATE)) case(s) bit-exact"

# The honest picture. Always exits 0 -- it is a REPORT, and a report that
# fails is a report nobody runs. Bisect with the byte counts, never with
# "the first mismatch moved", which says nothing.
test-vp9-diff: $(BUILD)/vp9_test
	@mkdir -p $(BUILD)/vp9corpus
	@bash tools/genvp9.sh $(BUILD)/vp9corpus >/dev/null 2>&1 || exit 0
	@for f in $(BUILD)/vp9corpus/*.ivf; do \
	    b=`basename $$f .ivf`; \
	    printf '%-26s ' "$$b"; \
	    $(BUILD)/vp9_test --diff $$f $(BUILD)/vp9corpus/$$b.ref.yuv 2>&1 | tail -1; \
	 done
