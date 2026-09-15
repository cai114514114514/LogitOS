# tests/mpeg4.mk -- the MPEG-4 Part 2 / H.263 decoder's gates.
#
# Kept out of the main Makefile on purpose, the same arrangement
# tests/h265.mk, tests/vp9.mk and tests/legacy.mk already use.
#
# ============================================================================
# READ THIS FIRST: THE DECODER CANNOT DECODE A SINGLE FRAME, ON ANYTHING.
# ============================================================================
# This is not "0 of N cases pass" the way tests/vp9.mk's sibling gate is --
# it is worse, and the difference matters to a reader deciding what to trust.
# VP9's decoder runs end to end and gets every pixel wrong; MPEG-4's decoder
# has no end to end AT ALL. Two pieces were never written:
#
#   1. c/lib/video/mpeg4_int.h (line 146, "/* mpeg4_mb.c */") declares TEN
#      functions as living in a file called mpeg4_mb.c -- m4_decode_mb,
#      m4_decode_mb_h263, m4_decode_mb_partitioned, m4_decode_partitions,
#      m4_is_resync, m4_clean_buffers, m4_clean_intra_entries,
#      m4_update_motion_val, m4_pred_motion, m4_dequant. That file DOES NOT
#      EXIST anywhere in this tree (`ls c/lib/video/mpeg4_mb.c` ->
#      "No such file or directory", `grep -rn "int m4_decode_mb("` over
#      every .c file in the tree -> zero matches). This is the macroblock
#      layer -- MCBPC/CBPY/MVD/TCOEF VLC decode, motion vector prediction,
#      dequantisation -- and none of it exists.
#   2. No file anywhere defines mpeg4_open, mpeg4_decode, mpeg4_decode_pts,
#      mpeg4_flush, mpeg4_close or mpeg4_last_error -- the six functions
#      mpeg4.h declares as this decoder's ENTIRE public API. Grepped, zero
#      definitions. There is no bitstream scanner, no VOS/VO/VOL dispatch
#      loop, no frame pool, no way to call this code from outside itself.
#
# Proof, not assertion -- a 3-line probe that only calls mpeg4_open() against
# the three source files that DO exist:
#
#   $ gcc -Ic/lib/video -o probe probe.c mpeg4_hdr.c mpeg4_idct.c mpeg4_mc.c
#   ld.bfd: probe.o: in function `main':
#   probe.c:(.text+0x12): undefined reference to `mpeg4_open'
#
# What DOES exist, compiles cleanly, and is genuinely well-built: the VOL/VOP
# HEADER LAYER (mpeg4_hdr.c, 836 lines -- every refusal named, argued in its
# own header comment), the IDCT (mpeg4_idct.c, pinned bit-exact to FFmpeg's
# simple IDCT, verified below), motion compensation / interpolation
# (mpeg4_mc.c, 716 lines, half-pel + quarter-pel + the H.263 loop filter),
# and 767 lines of mechanically-generated VLC tables (mpeg4_tables.h). Three
# of five pieces, and the two missing ones are the ones that call the other
# three per macroblock and hand a decoded picture to a caller. mpeg4_hdr.c
# and mpeg4_mc.c themselves call NONE of the ten missing symbols (grepped),
# so what compiles today is inert: correct code with nothing driving it.
#
# NOT MY FILE TO WRITE. mpeg4_mb.c and a top-level orchestrator are not in
# this phase's file list, and writing an untested ~1000-line macroblock
# decoder under the banner of "verification" would replace one honest
# finding (a decoder that does not exist yet) with one unverified guess
# dressed as progress. This fragment reports the gap with evidence and gates
# the two pieces that ARE self-contained and callable without it.
#
# ============================================================================
# THE BAR, AND WHY IT DIFFERS FROM tests/legacy.mk's
# ============================================================================
# mpeg4_idct.c's own header states it: ISO/IEC 14496-2 Annex A gives only a
# STATISTICAL accuracy bound (the IEEE 1180 tests), not a bit-exact
# transform, so two conforming decoders may legitimately differ by +/-1 per
# sample and drift along a GOP. "Within a tolerance" is not a bar this tree
# accepts for anything it CAN pin, so the transform is pinned: bit-exact
# against FFmpeg's ff_simple_idct_int16_8bit ("-idct simple -cpuflags 0" on
# every reference decode, per that header). tests/unit/mpeg4_idct_ref.c is
# that oracle -- a SEPARATE transcription of
# build/ffmpeg-8.0.1/libavcodec/simple_idct_template.c's BIT_DEPTH==8
# branch, not a copy of mpeg4_idct.c and not a call into it. It was
# additionally cross-checked ONCE, by hand, against the ACTUAL COMPILED
# FFmpeg object (build/ffmpeg-8.0.1/libavcodec/simple_idct.o -- `nm -u`
# shows zero undefined symbols, so it links standalone) over 6,014 cases /
# 384,896 samples: agreed byte for byte on every one. That object file is a
# local build artifact (build/ is gitignored) and is not relied on by the
# committed gate below, which runs against the transcription instead -- the
# cross-check is recorded here because it is what makes trusting the
# transcription itself justified, not assumed.
#
# Motion compensation has NO comparable ambiguity to resolve -- half-pel and
# quarter-pel interpolation are both exactly specified integer arithmetic
# with no rounding left to the implementer beyond the ONE bit the bitstream
# itself carries (rounding_type) -- so tests/unit/mpeg4_mc_test.c holds it
# to the same bit-exact bar via an independently written reference, no
# tolerance either.

MPEG4_IDCT_SRC := c/lib/video/mpeg4_idct.c
MPEG4_MC_SRC   := c/lib/video/mpeg4_mc.c
# NOTE the include path: -Ic/lib/video ONLY. $(INCDIRS) or
# -Ic/apps/libc/include breaks a HOST gcc build -- mini-libc's features.h
# shadows glibc's and __GLIBC_USE(X) then parses as a call. Same trap
# tests/vp9.mk and tests/legacy.mk document; it cost a compile here too
# before this comment existed.
MPEG4_INC := -Ic/lib/video

.PHONY: test-mpeg4 test-mpeg4-idct test-mpeg4-idct-negctl \
        test-mpeg4-mc test-mpeg4-mc-negctl test-mpeg4-units-asan

# The top-level target. Reports the gap above rather than gating on it --
# exiting nonzero here would be a FALSE failure (nothing is broken; nothing
# was ever finished), and the vp9.mk precedent is exactly this: "this target
# passes because it claims nothing." A `make test` sweep that reached this
# target learns the true state instead of either a false red or a false
# green.
test-mpeg4:
	@echo "MPEG4: mpeg4_mb.c (the macroblock/VLC layer) and the top-level"
	@echo "       mpeg4_open/mpeg4_decode orchestrator do not exist. This"
	@echo "       decoder cannot decode a single frame on anything -- see"
	@echo "       the header of tests/mpeg4.mk for the evidence. Nothing"
	@echo "       is claimed here. Run test-mpeg4-idct and test-mpeg4-mc"
	@echo "       for what DOES exist and IS measured."
	@echo "MPEG4-GAP-OK (reported, not gated)"

# --- the IDCT: bit-exact, real oracle, real corpus --------------------------
$(BUILD)/mpeg4_idct_test: tests/unit/mpeg4_idct_test.c tests/unit/mpeg4_idct_ref.c \
                          $(MPEG4_IDCT_SRC) c/lib/video/mpeg4_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/mpeg4_idct_test.c tests/unit/mpeg4_idct_ref.c \
	    $(MPEG4_IDCT_SRC) $(MPEG4_INC)

# 2,678 cases (every impulse position at 10 magnitudes, DC-only spanning the
# full int16 range including values that trip the row-shortcut's 16-bit
# wrap, 800 mid-magnitude random blocks, 400 full-int16-range adversarial
# blocks, 400 sparse blocks, 400 realistic decaying-magnitude blocks) x 64
# samples x {put,add} = 342,784 samples total. Measured 2026-08-25:
# 0/342,784 wrong. Bit-exact, not "close".
test-mpeg4-idct: $(BUILD)/mpeg4_idct_test
	@./$(BUILD)/mpeg4_idct_test

# The negative control: MPEG4_IDCT_CONTROL_W4_16384 uses 16384 -- the value
# the cosine formula cos(4*pi/16)*sqrt(2)*2^14 actually rounds to, and the
# value anyone deriving the constant from the mathematics rather than from
# the pinned oracle would write down. mpeg4_idct.c's own header names this
# choice explicitly (note 1) as one of three deliberate departures from "the
# obvious" value. Not every case reddens -- only where a column's true value
# sits within 32 units of a rounding boundary ahead of the final >>20 --
# which is why the count is neither 0 nor everything.
#
# THE COUNT IS COMPILER-DEPENDENT, and only in this deliberately-wrong
# configuration -- worth recording plainly rather than picking one number
# and hiding the rest. clang (this tree's $(CC)) gives 1897/171392 put,
# 2012/171392 add; gcc on the same source and the same host gives
# 1965/171392 and 2013/171392. The REAL, shipped path (W4=16383, no control
# macro) is byte-for-byte IDENTICAL between the two compilers -- 0/171392
# both -- so this is a property of the synthetic wrong constant landing on
# implementation-defined rounding at a handful of the corpus's boundary
# cases, not a compiler-dependent bug in anything this tree ships. The
# assertion below is pinned to clang's number because that is what
# `make test-mpeg4-idct-negctl` actually runs.
$(BUILD)/mpeg4_idct_test_ctl: tests/unit/mpeg4_idct_test.c tests/unit/mpeg4_idct_ref.c \
                              $(MPEG4_IDCT_SRC) c/lib/video/mpeg4_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DMPEG4_IDCT_CONTROL_W4_16384 -o $@ tests/unit/mpeg4_idct_test.c \
	    tests/unit/mpeg4_idct_ref.c $(MPEG4_IDCT_SRC) $(MPEG4_INC)

test-mpeg4-idct-negctl: $(BUILD)/mpeg4_idct_test_ctl
	@out=`./$(BUILD)/mpeg4_idct_test_ctl 2>&1 || true`; \
	 echo "$$out"; \
	 put=`echo "$$out" | sed -n 's/.*put: \([0-9]*\)\/.*/\1/p'`; \
	 add=`echo "$$out" | sed -n 's/.*add: \([0-9]*\)\/.*/\1/p'`; \
	 if [ "$$put" != "1897" ] || [ "$$add" != "2012" ]; then \
	    echo "MPEG4-IDCT-CONTROL-FAIL: put=$$put (want 1897) add=$$add (want 2012)"; exit 1; \
	 fi; \
	 echo "MPEG4-IDCT-CONTROL-OK: put=1897/171392 add=2012/171392 (clang; see the comment above on why this differs from gcc)"

# --- motion compensation: half-pel bit-exact, quarter-pel smoke-tested only -
# tests/unit/mpeg4_mc_test.c's own header states the scope precisely: LUMA
# only, M4_MV_16X16 only, quarter_sample=0 only. Chroma, four-MV,
# field/interlaced and 15 of 16 quarter-pel composition cases are NOT
# independently verified -- named there and here rather than left to be
# discovered by a passing gate that implies more than it measures.
#
# Measured 2026-08-25: 0/5,120 half-pel luma samples wrong across 10
# geometry cases (every dxy 0..3, fully in-bounds blocks, and blocks pushed
# past all four picture edges to exercise edge_mc's border replication) x 2
# roundings, plus a 0/256 quarter-pel mc00 (no fractional offset) identity
# check.
$(BUILD)/mpeg4_mc_test: tests/unit/mpeg4_mc_test.c $(MPEG4_MC_SRC) c/lib/video/mpeg4_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/mpeg4_mc_test.c $(MPEG4_MC_SRC) $(MPEG4_INC)

test-mpeg4-mc: $(BUILD)/mpeg4_mc_test
	@./$(BUILD)/mpeg4_mc_test

# The negative control: MPEG4_HPEL_CONTROL_NO_ROUND drops the rounding_type
# bit's effect on half-pel interpolation entirely -- mpeg4_mc.c's own header
# names this exact failure mode ("a decoder that ignores the bit still
# produces a plausible picture that is wrong by one step almost
# everywhere"). Measured 2026-08-25, exact and reproducible: 506/5120
# samples wrong (dxy==0, full-pel copy, is unaffected by construction, which
# is why it is not all 5120).
#
# tests/mpeg4.mk does NOT add a second control for
# -DMPEG4_QPEL_NO_ROUND (already present in mpeg4_mc.c, cited in its own
# header) -- this gate's only qpel case is the dxy=0 identity check, which
# that switch cannot affect, so asserting a count against it here would be
# asserting zero and calling it a control. It remains a real, wired switch
# for whoever extends the quarter-pel coverage this file names as missing.
$(BUILD)/mpeg4_mc_test_ctl: tests/unit/mpeg4_mc_test.c $(MPEG4_MC_SRC) c/lib/video/mpeg4_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DMPEG4_HPEL_CONTROL_NO_ROUND -o $@ tests/unit/mpeg4_mc_test.c \
	    $(MPEG4_MC_SRC) $(MPEG4_INC)

test-mpeg4-mc-negctl: $(BUILD)/mpeg4_mc_test_ctl
	@out=`./$(BUILD)/mpeg4_mc_test_ctl 2>&1 || true`; \
	 echo "$$out"; \
	 n=`echo "$$out" | sed -n 's/.*luma): \([0-9]*\)\/.*/\1/p'`; \
	 if [ "$$n" != "506" ]; then \
	    echo "MPEG4-MC-CONTROL-FAIL: $$n/5120 reddened, want 506"; exit 1; \
	 fi; \
	 echo "MPEG4-MC-CONTROL-OK: 506/5120 reddened, exactly as measured"

# Both modules under ASan/UBSan. mpeg4_mc.c indexes a padded emulation
# buffer by hand (edge_mc) -- precisely the shape of code that reads one
# sample too far without ever producing a visibly wrong pixel on a host
# build.
test-mpeg4-units-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    -o $(BUILD)/mpeg4_idct_test_asan tests/unit/mpeg4_idct_test.c \
	    tests/unit/mpeg4_idct_ref.c $(MPEG4_IDCT_SRC) $(MPEG4_INC)
	@UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/mpeg4_idct_test_asan
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    -o $(BUILD)/mpeg4_mc_test_asan tests/unit/mpeg4_mc_test.c $(MPEG4_MC_SRC) $(MPEG4_INC)
	@UBSAN_OPTIONS=halt_on_error=1 ./$(BUILD)/mpeg4_mc_test_asan
