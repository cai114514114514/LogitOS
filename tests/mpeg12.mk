# tests/mpeg12.mk -- the MPEG-1/MPEG-2 video decoder's gates.
#
# Kept out of the main Makefile on purpose, same arrangement as tests/h265.mk,
# tests/vp9.mk, tests/subs.mk etc: the root Makefile carries one
# `-include tests/mpeg12.mk` line and nothing else.
#
# THE BAR, ALREADY DECIDED BY THE SOURCE -- this fragment follows it rather
# than inventing one. c/lib/video/mpeg12.c's own header and tools/genmpeg12.sh
# both argue it: ISO/IEC 13818-2 Annex A specifies the inverse DCT only by an
# IEEE 1180 ACCURACY requirement, not an algorithm, so two conforming decoders
# may legitimately differ by +-1 per sample -- and because P/B pictures predict
# from the reconstruction, that difference would otherwise accumulate over a
# whole GOP. So the transform is PINNED: c/lib/video/mpeg12_idct.c implements
# the exact integer transform FFmpeg's `-idct simple` selects, the reference
# YUV in this fragment is generated with that same flag
# (tools/genmpeg12.sh:8-16), and with the transform pinned on both sides every
# remaining difference is a real decoding difference. The gate below is
# therefore BIT-EXACTNESS over whole streams, the same bar as H.264/H.265 --
# not a tolerance, because a tolerance here would be a decision to stop
# measuring (see the H.264/H.265 gates' own framing, and mpeg12.c:26-36 for the
# control that justifies pinning the flag at all: two idct flags on the SAME
# ffmpeg decode differ in 18.1% of samples).
#
# THE ONE BUG FOUND HERE WAS IN THE TEST, NOT THE DECODER, AND THAT DISTINCTION
# MATTERS -- CLAUDE.md has a whole section on failures that were the apparatus
# and not the system, and this is one more for that list. First run of
# tests/unit/mpeg12_idct_test.c failed all 64 `test_butterfly` checks, every
# `got` value exactly sqrt(2) times `want`. The formula computed the expected
# 1D response for a coefficient placed at 2D position (v=0, u=k) as
# `AMP * C(k) * cos(...) / 8.0`, omitting the C(0) row-direction normalisation
# factor the full 2D IDCT definition requires at v=0. Verified independently
# three ways before touching anything: (1) hand-derivation of the correct 2D
# formula from the same definition test_ieee1180() already uses; (2) a Python
# check reproducing got/want == sqrt(2) exactly at several (k,n) pairs;
# (3) test_ieee1180() in the SAME file already used the full, correctly
# normalised C(u)*C(v) 2D reference and PASSED at peak error <=1 -- proof the
# decoder's transform was already correct and only the simplified butterfly
# formula was wrong. Fixed to `AMP * C(k) * C(0) * cos(...) / 4.0`; re-ran:
# MPEG12-IDCT-OK, butterfly worst |err| 1, matching the (unchanged, still
# passing) IEEE 1180 numbers. No decoder code changed for this fix.
#
# THE CORPUS IS 31 STREAMS, GENERATED, NEVER COMMITTED: 26 from ffmpeg's own
# mpeg1video/mpeg2video encoders (tools/genmpeg12.sh) plus 5 hand-assembled by
# tools/genmpeg12_interlaced.py for syntax ffmpeg's encoder cannot write --
# field pictures, 16x8 field MC, and dual-prime. `ls build/mpeg12corpus/*.m2v
# | wc -l` and the pass-count script both independently read 31/31; a corpus
# whose size disagreed between two runs would be reported here as a fault, not
# glossed over -- it does not.
#
# ALL 31 CASES DECODE BIT-EXACT against the pinned ffmpeg reference, cross-
# checked with mpeg12_get_census() so the exotic-feature counts are known
# nonzero on the case meant to exercise each: mv_16x8=89 (field-16x8),
# mv_dualprime=130/131 (field-dmv/frame-dmv), field_dct up to 230
# (m2-ildct-*), esc2=1915 (m1-esc2-352x288, MPEG-1's second escape code),
# intra_vlc_blocks=2406 (m2-intravlc-352x288). A census that read zero on the
# case meant to exercise a feature would mean the case was not exercising it;
# none did.
#
# THE NEGATIVE CONTROL, modelled on tests/subs.mk's shape (an independent
# oracle -- ffmpeg's own decoder -- plus a control that is the PLAUSIBLE WRONG
# implementation, not a mutilation, and asserted by BOTH its count and its
# exact membership, not just "something failed"). c/lib/video/mpeg12_idct.c's
# own header names three implementation CHOICES a "cleaner" rewrite would get
# wrong; choice #2, the row pass's DC-only shortcut, is wired to
# -DMPEG12_CONTROL_NO_DC_SHORTCUT. Run against all 31 cases: 29 of 31 redden,
# maxd=1 on every one (exactly the algebraic story -- the shortcut and the
# general path disagree by <=1 before the column pass, see the derivation in
# mpeg12_idct.c and the "2048 of 4096" count in mpeg12_idct_test.c), and the
# TWO that do not redden are m1-esc2-352x288 and m2-esc-352x288 -- the two
# random-noise/escape-code cases, where flat/DC-shortcut-eligible blocks are
# apparently rare enough (or land where the two paths agree) that this
# particular corpus never exercises the divergence there. That is reported
# rather than hidden: the control's exact membership is asserted below, not
# rounded up to "the control works".

MPEG12_SRC := c/lib/video/mpeg12.c c/lib/video/mpeg12_idct.c \
              c/lib/video/mpeg12_mc.c c/lib/video/mpeg12_slice.c \
              c/lib/video/mpeg12_tables.c
MPEG12_INC := -Ic/lib/video

# NOTE the include path: -Ic/lib/video ONLY, same trap as tests/h265.mk and
# tests/vp9.mk document -- $(INCDIRS) or -Ic/apps/libc/include shadows glibc's
# features.h with mini-libc's and __GLIBC_USE(X) then parses as a call.

$(BUILD)/mpeg12_test: tests/unit/mpeg12_test.c $(MPEG12_SRC) c/lib/video/mpeg12.h \
                      c/lib/video/mpeg12_int.h c/lib/video/mpeg12_bits.h \
                      c/lib/video/mpeg12_tables.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/mpeg12_test.c $(MPEG12_SRC) $(MPEG12_INC)

$(BUILD)/mpeg12_idct_test: tests/unit/mpeg12_idct_test.c c/lib/video/mpeg12_idct.c \
                           c/lib/video/mpeg12_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/mpeg12_idct_test.c \
	    c/lib/video/mpeg12_idct.c $(MPEG12_INC) -lm

# The control build: -DMPEG12_CONTROL_NO_DC_SHORTCUT compiles OUT the row
# pass's DC-only shortcut (mpeg12_idct.c's documented choice #2), forcing
# every row through the general path on the theory that the shortcut is "just"
# an optimisation -- which mpeg12_idct.c's own header already argues is wrong.
$(BUILD)/mpeg12_test_control: tests/unit/mpeg12_test.c $(MPEG12_SRC) c/lib/video/mpeg12.h \
                              c/lib/video/mpeg12_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DMPEG12_CONTROL_NO_DC_SHORTCUT -o $@ \
	    tests/unit/mpeg12_test.c $(MPEG12_SRC) $(MPEG12_INC)

# The cases `make test-mpeg12` requires to be BIT-EXACT. All 31 generated
# cases are in it -- see the header above for the corpus count and the census
# cross-check that says each named feature is genuinely exercised, not merely
# present-but-empty.
MPEG12_GATE := field-16x8 field-dmv field-intra frame-dmv frame-field \
               m1-cqm-352x288 m1-esc2-352x288 m1-g30-352x288 m1-i-352x288 \
               m1-ibbp-322x242 m1-ibbp-352x288 m1-ip-352x288 m1-q1-352x288 \
               m2-640x360 m2-altscan-352x288 m2-cqm-352x288 m2-esc-352x288 \
               m2-i-322x242 m2-i-352x288 m2-ibbp-322x242 m2-ibbp-352x288 \
               m2-ibbp-g30 m2-ibbp-g4 m2-ildct-352x288 m2-ildct-alt-352x288 \
               m2-ildct-bf-352x288 m2-intravlc-352x288 m2-ip-352x288 \
               m2-nonlinq-352x288 m2-q1-352x288 m2-q25-352x288

.PHONY: test-mpeg12 test-mpeg12-diff test-mpeg12-idct test-mpeg12-negctl \
        test-mpeg12-census

# --- whole-stream gate -------------------------------------------------------
# Bit-exact against ffmpeg's own decode with the transform pinned (see header).
# genmpeg12.sh has no explicit missing-encoder exit code the way
# genvideo265.sh returns 3 for a missing libx265 -- ffmpeg's mpeg1video/
# mpeg2video encoders are built into essentially every ffmpeg, unlike libx265,
# so any nonzero exit from the generator here is treated the same way: skip
# with an explanation, do not fail the build over an absent encoder.
test-mpeg12: $(BUILD)/mpeg12_test
	@mkdir -p $(BUILD)/mpeg12corpus
	@rc=0; bash tools/genmpeg12.sh $(BUILD)/mpeg12corpus >/dev/null 2>&1 || rc=$$?; \
	 if [ $$rc != 0 ]; then \
	    echo "MPEG12: could not generate the corpus (no ffmpeg mpeg1video/mpeg2video?) -- nothing measured"; \
	    exit 0; \
	 fi; \
	 for c in $(MPEG12_GATE); do \
	    $(BUILD)/mpeg12_test $(BUILD)/mpeg12corpus/$$c.m2v \
	        $(BUILD)/mpeg12corpus/$$c.ref.yuv || exit 1; \
	 done; \
	 echo "MPEG12-OK $(words $(MPEG12_GATE)) case(s) bit-exact"

# The honest picture. Always exits 0 -- it is a REPORT, and a report that
# fails is a report nobody runs. Bisect with the byte counts, never with "the
# first mismatch moved", which says nothing (tests/unit/mpeg12_test.c's own
# header makes the same argument).
test-mpeg12-diff: $(BUILD)/mpeg12_test
	@mkdir -p $(BUILD)/mpeg12corpus
	@bash tools/genmpeg12.sh $(BUILD)/mpeg12corpus >/dev/null 2>&1 || exit 0
	@for f in $(BUILD)/mpeg12corpus/*.m2v; do \
	    b=`basename $$f .m2v`; \
	    printf '%-24s ' "$$b"; \
	    $(BUILD)/mpeg12_test --diff $$f $(BUILD)/mpeg12corpus/$$b.ref.yuv 2>&1 | tail -1; \
	 done

# The IDCT alone: is it an IDCT (butterfly coefficients vs cos((2n+1)k*pi/16)),
# is it accurate enough to be conforming (IEEE 1180 / Annex A, run in full),
# and is the DC shortcut a real behavioural difference and not a no-op. Fast,
# no corpus needed.
test-mpeg12-idct: $(BUILD)/mpeg12_idct_test
	@$(BUILD)/mpeg12_idct_test

# The negative control. Asserted on BOTH halves, same shape tests/subs.mk uses
# for its control: not just "something failed", but the EXACT set that must
# fail and the exact set that must not. See the header above for why those two
# cases are expected to stay clean -- it is a measured property of this
# corpus, not assumed.
test-mpeg12-negctl: $(BUILD)/mpeg12_test_control
	@mkdir -p $(BUILD)/mpeg12corpus
	@rc=0; bash tools/genmpeg12.sh $(BUILD)/mpeg12corpus >/dev/null 2>&1 || rc=$$?; \
	 if [ $$rc != 0 ]; then \
	    echo "MPEG12: no corpus -- negctl not run"; exit 0; \
	 fi; \
	 clean=""; red=0; \
	 for c in $(MPEG12_GATE); do \
	    if $(BUILD)/mpeg12_test_control $(BUILD)/mpeg12corpus/$$c.m2v \
	        $(BUILD)/mpeg12corpus/$$c.ref.yuv >/dev/null 2>&1; then \
	        clean="$$clean $$c"; \
	    else \
	        red=$$((red + 1)); \
	    fi; \
	 done; \
	 want_clean="m1-esc2-352x288 m2-esc-352x288"; \
	 got_sorted=`echo $$clean | tr ' ' '\n' | sed '/^$$/d' | sort | tr '\n' ' '`; \
	 want_sorted=`echo $$want_clean | tr ' ' '\n' | sort | tr '\n' ' '`; \
	 echo "CONTROL-NO-DC-SHORTCUT: $$red of $(words $(MPEG12_GATE)) cases redden; clean:$$clean"; \
	 if [ "$$got_sorted" != "$$want_sorted" ]; then \
	    echo "CONTROL-MPEG12-FAIL: expected exactly {$$want_clean} to stay clean, got {$$clean}"; \
	    exit 1; \
	 fi; \
	 echo "CONTROL-MPEG12-OK: $$red of $(words $(MPEG12_GATE)) redden without the DC shortcut, exactly the noise/escape cases stay clean"

# The census, cross-checked against the case each field claims to exercise --
# mpeg12.h's own comment on mpeg12_census: "a control that reddens the whole
# suite is not measuring the one thing it claims", so this asserts EACH count
# against ITS case, not just that the sum is nonzero somewhere in the corpus.
test-mpeg12-census: $(BUILD)/mpeg12_test
	@mkdir -p $(BUILD)/mpeg12corpus
	@bash tools/genmpeg12.sh $(BUILD)/mpeg12corpus >/dev/null 2>&1 || { echo "MPEG12: no corpus -- census not run"; exit 0; }
	@fail=0; \
	 check() { \
	    v=`$(BUILD)/mpeg12_test --census $(BUILD)/mpeg12corpus/$$1.m2v | grep -o "$$2=[0-9]*" | cut -d= -f2`; \
	    if [ -z "$$v" ] || [ "$$v" -le 0 ]; then \
	        echo "CENSUS-MPEG12-FAIL: $$1 $$2=$$v (expected > 0)"; fail=1; \
	    else echo "  $$1 $$2=$$v"; fi; \
	 }; \
	 check field-16x8 mv_16x8; \
	 check field-dmv mv_dualprime; \
	 check frame-dmv mv_dualprime; \
	 check m2-ildct-352x288 field_dct; \
	 check m1-esc2-352x288 esc2; \
	 check m2-intravlc-352x288 intra_vlc_blocks; \
	 [ $$fail = 0 ] && echo "CENSUS-MPEG12-OK: every named feature is nonzero on its own case" || exit 1
