# tests/opus.mk -- the Opus decoder's gates.
#
# Kept out of the main Makefile on purpose, same reason as tests/h265.mk,
# tests/vp9.mk and the rest: this tree is worked on by several people at once
# and a shared file is where their edits collide. The root Makefile carries
# one `-include tests/opus.mk` line and nothing else.
#
# ============================================================================
# THE BAR, STATED EXACTLY, BECAUSE IT IS TWO DIFFERENT BARS FOR TWO HALVES OF
# ONE CODEC AND LOOSENING EITHER ONE IS THE EASY WAY TO GET THIS WRONG.
# ============================================================================
#
# THE RANGE DECODER (opus_range.c) IS EXACTLY SPECIFIED INTEGER ARITHMETIC,
# AND IT IS HELD TO BIT-EXACTNESS, MEASURED, NOT ASSUMED:
#
#   - test-opus-range: a second implementation of RFC 6716 4.1, transcribed
#     independently in tests/unit/opus_range_test.c, stepped in lockstep
#     against opus_range.c over pseudo-random buffers. 15,691 checks, 0
#     failures (2026-08-25).
#   - test-opus: every one of the 12 official RFC 6716 test vectors carries,
#     per packet, the ENCODER's own range-coder checksum. Measured: 20,075
#     packets across all 12 vectors, rng_mismatch=0 on EVERY ONE -- not just
#     the CELT-only vectors. 6,886 of those 20,075 packets belong to the 3
#     pure-CELT vectors (01/07/11), which is also the subset opus_compare can
#     score (see below) -- that number matches the one opus_range.h's own
#     header comment already cites, an independent cross-check that the
#     documentation there describes a real measurement.
#
#   This is a BIT-EXACT claim and it is stated as one. Nothing float touches
#   the checksum; it is uint32_t arithmetic end to end.
#
# THE CELT RECONSTRUCTION (opus_celt.c) IS NOT BIT-EXACT AND CANNOT BE, BY THE
# STANDARD'S OWN DESIGN -- see the long note at the top of opus.c: RFC 6716
# ships a float reference build and a fixed-point one, they disagree with each
# other in the low bits on the same packet, and the RFC declares BOTH
# conformant. Conformance is therefore defined by section 6 as agreement with
# the reference decoder's output under the reference's OWN `opus_compare`
# tool, not by byte equality. test-opus uses exactly that tool, unmodified,
# against the RFC's own reference `.dec` files, and treats its EXIT CODE as
# the verdict -- not a percentage threshold picked here. Confirmed by READING
# `build/.opus/ref/src/opus_compare.c` rather than trusting the tool's name:
# it computes `Q = 100*(1-0.5*log(1+err)/log(1.13))` and returns
# EXIT_FAILURE iff Q<0, EXIT_SUCCESS otherwise, printing "Test vector
# PASSES"/"FAILS" itself. That IS RFC 6716 section 6's conformance test.
#
#   Only the 3 pure-CELT vectors (01, 07, 11) are scored this way. The other
#   9 contain SILK or hybrid frames this decoder refuses BY NAME (see
#   opus_celt.h) and renders as silence instead -- scoring silence against
#   real reference audio would measure "is a placeholder aligned with the
#   reference", which tests/unit/opus_vec.c's own header already argues
#   against. Those 9 are instead pinned on their refused_silk/refused_hybrid
#   COUNTS (a regression check on opus.c's TOC mode dispatch) and on
#   rng_mismatch/hard_err over whatever CELT frames they do contain.
#
# ============================================================================
# tests/unit/opus_range_test.c WAS MUTE UNTIL 2026-08-25, AND IT MATTERS.
# ============================================================================
#
# The version this replaced defined a struct without a field its own update
# function referenced (a straight compile error) and had a `main()` that
# printed a banner and returned 0 without calling a single check -- the exact
# shape CLAUDE.md's test-suite section names MUTE: "computes a verdict and
# exits 0 anyway", the category that is supposed to be EMPTY. It has been
# rewritten as a real lockstep test (see its own header) and is no longer in
# that category.

OPUS_INC := -Ic/lib/audio
OPUS_SRC := c/lib/audio/opus.c c/lib/audio/opus_range.c c/lib/audio/opus_celt.c \
            c/lib/audio/afft.c c/lib/audio/amath.c
OPUS_HDR := c/lib/audio/opus.h c/lib/audio/opus_range.h c/lib/audio/opus_celt.h \
            c/lib/audio/opus_tables.h c/lib/audio/afft.h c/lib/audio/amath.h

# NOTE the include path: -Ic/lib/audio ONLY. $(INCDIRS) or -Ic/apps/libc/include
# puts mini-libc's features.h ahead of glibc's on a HOST build, and
# __GLIBC_USE(X) then parses as a call -- the same trap tests/vp9.mk and
# tests/h265.mk already document, paid again here before being caught.

$(BUILD)/opus_range_test: tests/unit/opus_range_test.c c/lib/audio/opus_range.c \
                          c/lib/audio/opus_range.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -w $(OPUS_INC) -o $@ \
	    tests/unit/opus_range_test.c c/lib/audio/opus_range.c

# The control build: drops RFC 6716 4.1.2's fl==0 special case in
# orange_update (see the #ifdef in opus_range.c for the full argument,
# including the earlier candidate -- the renormalization mask on `val` --
# that was tried and rejected because this decoder's own invariants make it
# unobservable).
$(BUILD)/opus_range_test_negctl: tests/unit/opus_range_test.c c/lib/audio/opus_range.c \
                                 c/lib/audio/opus_range.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DOPUS_RANGE_NEGCTL_NORM $(OPUS_INC) -o $@ \
	    tests/unit/opus_range_test.c c/lib/audio/opus_range.c

$(BUILD)/opus_vec: tests/unit/opus_vec.c $(OPUS_SRC) $(OPUS_HDR)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w $(OPUS_INC) -o $@ tests/unit/opus_vec.c $(OPUS_SRC) -lm

.PHONY: test-opus-range test-opus-range-negctl test-opus

test-opus-range: $(BUILD)/opus_range_test
	@$(BUILD)/opus_range_test

# PLAUSIBLE wrong implementation, not a mutilation: correct for every symbol
# with fl>0, wrong only for the one whose fl==0 (see opus_range.c). Watched
# failing, exact count, deterministic (every PRNG seed here is a fixed
# constant -- no time()-seeded run-to-run variance to chase).
test-opus-range-negctl: $(BUILD)/opus_range_test_negctl
	@out=`$(BUILD)/opus_range_test_negctl`; rc=$$?; \
	 last=`echo "$$out" | tail -1`; \
	 got=`echo "$$last" | sed -n 's/.* \([0-9][0-9]*\) failures/\1/p'`; \
	 if [ $$rc = 0 ]; then \
	    echo "CONTROL-FAIL: opus_range_test passed with the fl==0 special case removed"; \
	    exit 1; \
	 elif [ "$$got" != "6726" ]; then \
	    echo "CONTROL-FAIL: expected exactly 6726 reddened checks (of 15691), got $$got"; \
	    echo "$$last"; \
	    exit 1; \
	 else \
	    echo "CONTROL-OK: opus_range_test fails without the fl==0 special case ($$got of 15691 checks)"; \
	 fi

# The corpus is OPTIONAL and the capability is not -- same rule tests/wpt.mk
# already argues, and tools/opusvec.sh's own header states it for this one:
# `make opus-vectors` fetches; no test target here downloads anything, and a
# missing corpus exits 0 because that is not a regression in the decoder.
test-opus: $(BUILD)/opus_vec
	@if [ ! -d build/.opus/vectors ] || [ ! -x build/.opus/ref/opus_compare ]; then \
	    echo "OPUS: no build/.opus/{vectors,ref} -- corpus not fetched" ; \
	    echo "      (run: bash tools/opusvec.sh). Nothing measured." ; \
	    exit 0 ; \
	 fi
	@rc=0; \
	 for v in 01 02 03 04 05 06 07 08 09 10 11 12; do \
	    case $$v in \
	      02) esilk=1185; ehyb=0 ;; \
	      03) esilk=998;  ehyb=0 ;; \
	      04) esilk=1265; ehyb=0 ;; \
	      05) esilk=0;    ehyb=2037 ;; \
	      06) esilk=0;    ehyb=1876 ;; \
	      08) esilk=5;    ehyb=0 ;; \
	      09) esilk=5;    ehyb=0 ;; \
	      10) esilk=0;    ehyb=314 ;; \
	      12) esilk=1068; ehyb=264 ;; \
	      *)  esilk=0;    ehyb=0 ;; \
	    esac; \
	    out=`$(BUILD)/opus_vec build/.opus/vectors/testvector$$v.bit $(BUILD)/tv$$v.sw 2`; \
	    rng=`echo "$$out"  | grep -o 'rng_mismatch=[0-9]*'   | cut -d= -f2`; \
	    herr=`echo "$$out" | grep -o 'hard_err=[0-9]*'       | cut -d= -f2`; \
	    gsilk=`echo "$$out"| grep -o 'refused_silk=[0-9]*'   | cut -d= -f2`; \
	    ghyb=`echo "$$out" | grep -o 'refused_hybrid=[0-9]*' | cut -d= -f2`; \
	    if [ "$$rng" != "0" ] || [ "$$herr" != "0" ]; then \
	        echo "OPUS-FAIL vector$$v: rng_mismatch=$$rng hard_err=$$herr (want 0/0)"; \
	        echo "  $$out"; rc=1; continue; \
	    fi; \
	    if [ "$$gsilk" != "$$esilk" ] || [ "$$ghyb" != "$$ehyb" ]; then \
	        echo "OPUS-FAIL vector$$v: refused_silk=$$gsilk refused_hybrid=$$ghyb (want $$esilk/$$ehyb)"; \
	        echo "  $$out"; rc=1; continue; \
	    fi; \
	    echo "OPUS-OK vector$$v: $$out"; \
	 done; \
	 for v in 01 07 11; do \
	    cout=`build/.opus/ref/opus_compare -s build/.opus/vectors/testvector$$v.dec $(BUILD)/tv$$v.sw 2>&1`; \
	    ccode=$$?; \
	    if [ $$ccode != 0 ]; then \
	        echo "OPUS-FAIL vector$$v opus_compare (RFC 6716 sec.6 conformance): exit $$ccode"; \
	        echo "$$cout"; rc=1; \
	    else \
	        q=`echo "$$cout" | grep 'quality metric'`; \
	        echo "OPUS-OK vector$$v opus_compare PASSES -- $$q"; \
	    fi; \
	 done; \
	 exit $$rc
