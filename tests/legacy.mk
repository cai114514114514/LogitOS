# tests/legacy.mk -- the four archival codecs' gates: Cinepak, MS Video 1,
# RPZA, QuickTime Animation (QTRLE).
#
# Kept out of the main Makefile on purpose, the same arrangement tests/h265.mk
# and tests/vp9.mk already use: this tree is worked on by several people at
# once and a shared root Makefile is where their edits collide. The root
# Makefile carries one `-include tests/legacy.mk` line and nothing else.
#
# THE BAR: legacy.h's own header comment states it, and this file follows it
# exactly rather than restating it looser: "All four are deterministic
# BLOCK-REPLACEMENT schemes over a persistent frame buffer -- no transform,
# no entropy coder, no probability model, and (Cinepak's YUV->RGB convert
# aside) no rounding -- so 'matches ffmpeg's decoder byte-for-byte' is exactly
# the right bar." There is no tolerance anywhere below. A mismatch of one
# byte is a mismatch, full stop -- unlike tests/mpeg4.mk, where an IDCT with
# no bit-exact standard forces an explicit choice of oracle.
#
# BEFORE THIS FRAGMENT EXISTED, THESE FOUR DECODERS HAD NEVER RUN ONCE. Two
# separate things were true and neither was visible from outside:
#   1. No test source existed at all -- confirmed by grep, not assumed.
#   2. tools/genlegacy.py's __main__ called ONLY gen_cinepak(); gen_msvideo1
#      and gen_rpza were fully written (real ffmpeg differentials, their own
#      hand-authored 8-bit case, their own negative-control fixture) and
#      never once executed, and gen_qtrle did not exist at all. Running
#      gen_msvideo1 for the very first time (2026-08-25) immediately hit a
#      real bug in ITS OWN encoder helper (`_msvc_2color`'s caller built the
#      wrong bit pattern whenever the bottom-right pixel of a 2-color block
#      was NOT the `hi` color -- an assertion inside _msvc_2color caught it
#      on the very first block). Fixed in genlegacy.py; see its own comment
#      at the fix site. So "wire the dead code up" was not a formality here.

LEGACY_SRC := c/lib/video/legacy_cinepak.c c/lib/video/legacy_msvideo1.c \
              c/lib/video/legacy_qtrle.c c/lib/video/legacy_rpza.c
# NOTE the include path: -Ic/lib/video ONLY. $(INCDIRS) or -Ic/apps/libc/include
# breaks a HOST gcc build here -- mini-libc's features.h shadows glibc's and
# __GLIBC_USE(X) then parses as a call. The same trap tests/vp9.mk's own
# comment documents, and it cost a compile here before this comment existed.
LEGACY_INC := -Ic/lib/video
LEGACY_CORPUS := $(BUILD)/legacycorpus

# The bit-exact list. Every one of these decoded byte-for-byte identical to
# ffmpeg's own decoder on 2026-08-25 -- 38 of 38 positive cases, the first
# time any of this code had ever run. Nothing is held back here the way
# tests/vp9.mk holds back a wrong decoder: unlike VP9's sibling gate, there is
# no case below that is NOT in this list.
LEGACY_GATE := \
    cinepak-rgb24-16x16 cinepak-gray-16x16 cinepak-rgb24-32x32 cinepak-gray-32x32 \
    cinepak-rgb24-48x32 cinepak-gray-48x32 \
    msvideo1-16bit-16x16 msvideo1-16bit-32x32 msvideo1-16bit-48x32 msvideo1-8bit-16x16 \
    rpza-16x16 rpza-32x32 rpza-48x32 rpza-4color-4x4 \
    qtrle-16bit-16x16 qtrle-24bit-16x16 qtrle-32bit-16x16 qtrle-gray8-16x16 \
    qtrle-16bit-32x32 qtrle-24bit-32x32 qtrle-32bit-32x32 qtrle-gray8-32x32 \
    qtrle-16bit-48x32 qtrle-24bit-48x32 qtrle-32bit-48x32 qtrle-gray8-48x32 \
    qtrle-1bit-16x16 qtrle-1bit-32x32 qtrle-1bit-48x32 \
    qtrle-2bit-16x16 qtrle-2bit-32x32 qtrle-2bit-48x32 \
    qtrle-4bit-16x16 qtrle-4bit-32x32 qtrle-4bit-48x32 \
    qtrle-8bit-16x16 qtrle-8bit-32x32 qtrle-8bit-48x32

# One corrupted, standalone frame chunk per codec (never a whole container --
# genlegacy.py's own header explains why: the corruption is a single,
# structurally-motivated byte change in a REAL generated frame, not a
# mutilation). A fresh context must refuse every one of these with
# LEGACY_ERR_CORRUPT and must not crash; tests/unit/legacy_test.c's ASan
# build (test-legacy-asan below) is what stands behind "must not crash".
LEGACY_CORRUPT := cinepak-negctl msvideo1-negctl rpza-negctl qtrle-negctl

$(BUILD)/legacy_test: tests/unit/legacy_test.c $(LEGACY_SRC) c/lib/video/legacy.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)

$(LEGACY_CORPUS)/CASES: tools/genlegacy.py
	@mkdir -p $(LEGACY_CORPUS)
	@python3 tools/genlegacy.py $(LEGACY_CORPUS)

.PHONY: test-legacy test-legacy-diff test-legacy-negctl test-legacy-asan

test-legacy: $(BUILD)/legacy_test $(LEGACY_CORPUS)/CASES
	@for c in $(LEGACY_GATE); do \
	    ./$(BUILD)/legacy_test $(LEGACY_CORPUS) $$c || exit 1; \
	 done
	@for c in $(LEGACY_CORRUPT); do \
	    ./$(BUILD)/legacy_test $(LEGACY_CORPUS) $$c || exit 1; \
	 done
	@echo "LEGACY-OK $(words $(LEGACY_GATE)) case(s) bit-exact, $(words $(LEGACY_CORRUPT)) corrupt fixture(s) refused"

# The honest picture, and the number this tree bisects with: total wrong
# bytes per case, the first mismatching byte, and which frame carries the
# worst mismatch. Never gates -- always exits 0, exactly like
# tests/h265.mk's test-h265-diff and tests/vp9.mk's test-vp9-diff, for the
# same reason: a report that fails is a report nobody runs.
test-legacy-diff: $(BUILD)/legacy_test $(LEGACY_CORPUS)/CASES
	@for c in $(LEGACY_GATE) $(LEGACY_CORRUPT); do \
	    ./$(BUILD)/legacy_test --diff $(LEGACY_CORPUS) $$c; \
	 done

# --- negative controls: one real historical bug per codec, on a -D switch --
# Each is the PLAUSIBLE wrong implementation this exact shape of code
# invites, not a mutilation -- and each is verified against ffmpeg's own
# source before being written here (see the comment at each #ifdef site in
# c/lib/video/legacy_*.c). Every count below was WATCHED reddening at this
# exact number on 2026-08-25 and is asserted, not assumed.
$(BUILD)/legacy_test_cinctl: tests/unit/legacy_test.c $(LEGACY_SRC) c/lib/video/legacy.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DCINEPAK_CONTROL_ABS_Y1 -o $@ tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)
$(BUILD)/legacy_test_msvctl: tests/unit/legacy_test.c $(LEGACY_SRC) c/lib/video/legacy.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DMSVIDEO1_CONTROL_NOFLIP -o $@ tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)
$(BUILD)/legacy_test_rpzactl: tests/unit/legacy_test.c $(LEGACY_SRC) c/lib/video/legacy.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DRPZA_CONTROL_SWAPPED_BLEND -o $@ tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)
$(BUILD)/legacy_test_qtrctl: tests/unit/legacy_test.c $(LEGACY_SRC) c/lib/video/legacy.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DQTRLE_CONTROL_TICKET226 -o $@ tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)

test-legacy-negctl: $(BUILD)/legacy_test_cinctl $(BUILD)/legacy_test_msvctl \
                    $(BUILD)/legacy_test_rpzactl $(BUILD)/legacy_test_qtrctl \
                    $(LEGACY_CORPUS)/CASES
	@# Cinepak: CINEPAK_CONTROL_ABS_Y1 drops the "y1==0 means relative to
	@# the previous strip" rule ffmpeg's own cinepak.c carries at the
	@# identical field ("/* zero y1 means "relative to the previous
	@# stripe" */"). Wrong only on multi-strip content, which is why it
	@# is the gray/48x32-shaped cases and not every case -- 2 of 6.
	@n=0; for c in cinepak-rgb24-16x16 cinepak-gray-16x16 cinepak-rgb24-32x32 \
	    cinepak-gray-32x32 cinepak-rgb24-48x32 cinepak-gray-48x32; do \
	    ./$(BUILD)/legacy_test_cinctl $(LEGACY_CORPUS) $$c >/dev/null 2>&1 || n=$$((n+1)); \
	 done; \
	 if [ $$n != 2 ]; then echo "CINEPAK-CONTROL-FAIL: $$n/6 reddened, want 2"; exit 1; fi; \
	 echo "CINEPAK-CONTROL-OK: 2/6 reddened (multi-strip cases only)"
	@# MS Video 1: MSVIDEO1_CONTROL_NOFLIP drops the `^1` ffmpeg's own
	@# msvideo1.c XORs into every 2- and 8-color block's flag bit. Every
	@# case here uses 2- or 8-color blocks, so all 4 redden.
	@n=0; for c in msvideo1-16bit-16x16 msvideo1-16bit-32x32 msvideo1-16bit-48x32 \
	    msvideo1-8bit-16x16; do \
	    ./$(BUILD)/legacy_test_msvctl $(LEGACY_CORPUS) $$c >/dev/null 2>&1 || n=$$((n+1)); \
	 done; \
	 if [ $$n != 4 ]; then echo "MSVIDEO1-CONTROL-FAIL: $$n/4 reddened, want 4"; exit 1; fi; \
	 echo "MSVIDEO1-CONTROL-OK: 4/4 reddened"
	@# RPZA: RPZA_CONTROL_SWAPPED_BLEND swaps the 11/21 two-tap blend
	@# weights between the two derived colors of a 4-color block. Only
	@# rpza-4color-4x4 (hand-authored -- ffmpeg's OWN rpza encoder never
	@# chose the 4-color opcode for the other three cases' content,
	@# measured before this case was added) exercises that path: 1 of 4.
	@n=0; for c in rpza-16x16 rpza-32x32 rpza-48x32 rpza-4color-4x4; do \
	    ./$(BUILD)/legacy_test_rpzactl $(LEGACY_CORPUS) $$c >/dev/null 2>&1 || n=$$((n+1)); \
	 done; \
	 if [ $$n != 1 ]; then echo "RPZA-CONTROL-FAIL: $$n/4 reddened, want 1"; exit 1; fi; \
	 echo "RPZA-CONTROL-OK: 1/4 reddened (the hand-authored 4-color case)"
	@# QTRLE: QTRLE_CONTROL_TICKET226 undoes the real, named fix
	@# legacy_qtrle.c's own comment cites (trac.ffmpeg.org/ticket/226).
	@# It touches ONLY dec_1bpp, so exactly the three qtrle-1bit-* sizes
	@# redden and nothing else -- 3 of 24 (all non-negctl qtrle cases).
	@n=0; for c in $$(echo "$(LEGACY_GATE)" | tr ' ' '\n' | grep '^qtrle-'); do \
	    ./$(BUILD)/legacy_test_qtrctl $(LEGACY_CORPUS) $$c >/dev/null 2>&1 || n=$$((n+1)); \
	 done; \
	 if [ $$n != 3 ]; then echo "QTRLE-CONTROL-FAIL: $$n/24 reddened, want 3"; exit 1; fi; \
	 echo "QTRLE-CONTROL-OK: 3/24 reddened (the three qtrle-1bit-* sizes only)"

# Every input byte of an old media file is untrusted, per legacy.h's own
# header comment; this is what stands behind "must not crash" for the four
# corrupt fixtures test-legacy already requires LEGACY_ERR_CORRUPT from.
#
# detect_leaks=0, and this needs saying rather than quietly choosing it:
# with leak detection ON, `legacy_test_asan build/legacycorpus
# qtrle-24bit-16x16` HANGS (measured -- `timeout 30` fires, twice,
# reproducibly; the exact same invocation with detect_leaks=0 completes in
# well under a second, correctly). It is not this file's decoders leaking:
# every context is closed and every buffer freed in tests/unit/legacy_test.c
# before exit (read it -- there is no allocation left live at the point
# LeakSanitizer would scan), and `gdb --args` on the hang prints "LeakSanitizer
# does not work under ptrace" rather than a backtrace INSIDE this program,
# which is LSan's own stop-the-world leak scan, not decode logic, hanging.
# Heap-buffer-overflow/use-after-free/UB detection -- the property this
# target actually exists to check for four decoders reading attacker-
# controlled bytes -- does not depend on LeakSanitizer at all, so turning
# leak detection off costs this target nothing it was built to catch.
test-legacy-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    -o $(BUILD)/legacy_test_asan tests/unit/legacy_test.c $(LEGACY_SRC) $(LEGACY_INC)
	@$(MAKE) $(LEGACY_CORPUS)/CASES
	@rc=0; for c in $(LEGACY_GATE) $(LEGACY_CORRUPT); do \
	    out=`ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=print_stacktrace=1 \
	         ./$(BUILD)/legacy_test_asan $(LEGACY_CORPUS) $$c 2>&1`; \
	    if echo "$$out" | grep -q 'runtime error\|AddressSanitizer'; then \
	        echo "SANITIZER: $$c"; echo "$$out" | head -12; rc=1; \
	    fi; \
	 done; \
	 [ $$rc = 0 ] && echo "test-legacy-asan: clean over the whole matrix (memory-safety + UB only; see comment above on detect_leaks=0)" || exit 1
