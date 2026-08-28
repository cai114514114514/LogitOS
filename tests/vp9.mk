# VP9 -- the key-frame decoder, and an EMPTY exact list that says so.
#
# ===========================================================================
# READ THIS FIRST: THE DECODER IS NOT IN THIS TREE, AND THIS FRAGMENT IS THE
# ONLY SURVIVING RECORD THAT IT EVER RAN. Measured 2026-08-28.
# ===========================================================================
# `make test-vp9` answered "No rule to make target 'tests/unit/vp9_test.c'".
# That is not a broken path -- every file this gate names is absent, and none
# of them was ever committed:
#
#     find . -iname '*vp9*'                     -> tests/vp9.mk and one .webm
#     git log --all -- 'c/lib/video/vp9*' \
#                      tests/unit/vp9_test.c \
#                      tools/genvp9.sh          -> NOTHING, no commit, ever
#     git stash list                            -> empty
#
# The commit that landed this fragment (00a2aacfb, 2026-08-25) says so in as
# many words -- "THE DECODER ITSELF IS NOT IN THIS COMMIT. It is still
# uncommitted work from a line whose workflow died" -- so what shipped that day
# was the INSTRUMENT, deliberately, and the 14,274 lines it measures never
# followed it into git. The product agrees from the other end:
# `c/apps/gui/preview.c:28` lists "video is vp9" among the refusals it prints
# for a file with NO DECODER HERE. Two independent places in the tree say the
# same thing, which is what makes this a fact rather than a missing file.
#
# WHY THE INCLUDE STAYS AND THE TARGETS GUARD THEMSELVES. The other option was
# to drop `-include tests/vp9.mk` from the Makefile, and it is the worse one on
# three counts:
#
#   - The measurement below is the record. It is the only trace of the one run
#     that decoder ever had, and a fragment `make` no longer reads is a file
#     nobody opens. Deleting the include deletes the ratchet, which is exactly
#     the argument tests/wpt.mk:34-38 makes for its own absent corpus.
#   - The guard is SELF-HEALING. Presence is a $(wildcard), so the day the
#     sources land the real rules below are the ones defined -- with VP9_GATE
#     still empty -- and nobody has to remember to re-add an include. Same
#     principle tools/audit_tests.py states about asking the Makefile instead
#     of keeping a hand-written list of what CI runs.
#   - `make test-vp9` erroring on a missing prerequisite reads like a build
#     breakage in the media line. It is not one. It is a decoder that is not
#     here, and saying that costs one line.
#
# AND THE DIFFERENCE FROM WPT'S ABSENT CORPUS IS STATED, NOT GLOSSED: what is
# missing there is DATA and what is missing here is THE CODE UNDER TEST. So the
# message must not read as a skipped test. It names the absent files and says
# no checkout brings them back, because "the corpus did not generate" and "the
# decoder does not exist" are different findings and only one of them is true.
#
# tools/genvp9.sh IS IN THE PRESENCE CHECK ON PURPOSE. Without that, the
# existing recipe's `bash tools/genvp9.sh || ...` fallback would catch a
# missing GENERATOR and print "no ffmpeg with libvpx-vp9" -- blaming the host
# for a file that is not in the repo. That is the shape CLAUDE.md names as the
# expensive one: the measurement was right and the sentence around it sent the
# reader somewhere else. The message below lists what is absent BY NAME, so it
# cannot misattribute whichever half is missing.
#
# ---------------------------------------------------------------------------
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
VP9_HDRS := c/lib/video/vp9.h c/lib/video/vp9_int.h c/lib/video/vp9_tables.h

# Everything this gate cannot run without, and it is checked as a SET rather
# than by probing one witness file: a half-restored decoder must report which
# half, not "vp9.c is there so go ahead" followed by a link error.
VP9_NEEDS   := tests/unit/vp9_test.c tools/genvp9.sh $(VP9_SRC) $(VP9_HDRS)
VP9_MISSING := $(strip $(foreach f,$(VP9_NEEDS),$(if $(wildcard $(f)),,$(f))))

# Nothing is bit-exact yet. When a case becomes exact it is added here, and
# from that moment a regression in it fails the build.
VP9_GATE :=

.PHONY: test-vp9 test-vp9-diff

ifeq ($(VP9_MISSING),)

# NOTE the include path: -Ic/lib/video ONLY. Adding $(INCDIRS) or
# -Ic/apps/libc/include breaks a HOST gcc build, because mini-libc's features.h
# shadows glibc's and __GLIBC_USE(X) then parses as a call. That is the header
# basename collision CLAUDE.md documents for the freestanding build, and it
# bites host builds too -- it cost a compile here before it was recognised.
$(BUILD)/vp9_test: tests/unit/vp9_test.c $(VP9_SRC) $(VP9_HDRS)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(VP9_INC) -o $@ tests/unit/vp9_test.c $(VP9_SRC)

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

else

# THE ABSENT-DECODER BRANCH. Loud, specific, and exit 0 -- a decoder that was
# never committed is not a regression in anything that WAS. Note what this does
# NOT do: it defines no $(BUILD)/vp9_test rule, so no target depends on a file
# that cannot be made, and it weakens no assertion, because VP9_GATE is empty
# and this gate asserts nothing in either branch. The day the files land the
# ifeq above takes the other arm and the real rules are back unchanged.
define VP9_ABSENT_SAY
@echo "VP9: the decoder is NOT IN THIS TREE. Nothing was built and nothing measured."
@echo "     $(words $(VP9_MISSING)) of the $(words $(VP9_NEEDS)) files this gate names are absent:"
@for f in $(VP9_MISSING); do echo "       $$f"; done
@echo "     They were never committed -- git log --all over those paths returns"
@echo "     nothing -- so no checkout, fetch or generator restores them. Only"
@echo "     writing or recovering c/lib/video/vp9*.c changes this line."
@echo "     tests/vp9.mk's header holds the 17-case matrix from the one run that"
@echo "     decoder ever had (2026-08-25); it is the record, read it first."
endef

test-vp9:
	$(VP9_ABSENT_SAY)

test-vp9-diff:
	$(VP9_ABSENT_SAY)

endif
