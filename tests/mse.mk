# tests/mse.mk -- Media Source Extensions: what builds them and what gates them.
#
# Kept out of the root Makefile for the reason tests/demux.mk states: this tree
# is worked on by several lines at once and a shared 2600-line file is where
# their edits collide. The root Makefile carries exactly ONE line for all of
# this -- `-include tests/mse.mk` -- and nothing else: the browser's link, the
# disk contents and every target below are arranged from here. See the
# BROWSER_OBJ note further down for how the link is reached without touching
# the recipe.
#
# WHAT IS BEING GATED. Two claims that fail in opposite directions:
#
#   isTypeSupported() is true BOTH WAYS. Every type the browser says yes to is
#   decoded here from a real file; every type it says no to is asserted to say
#   no, av01 by name. That answer is not a formality: it is the mechanism by
#   which a real DASH site decides what to send us, so an honest "no" to AV1 is
#   what makes bilibili serve the H.264 it also offers.
#
#   A DASH-shaped segmented stream plays. Init segment plus numbered .m4s,
#   video and audio in separate files, appended over simulated time -- and the
#   pictures must arrive in presentation order, match the whole-file decode
#   sample for sample, and hold an A/V drift bound measured by avclock itself.

.PHONY: test-mse test-mse-asan test-mse-negctl test-mse-nocard-negctl test-mse-os mse-fixtures

MSE_FX   := tests/fixtures/mse
MSE_SRC  := c/apps/browser/js_media_src.c
MSE_HDRS := c/apps/browser/js_media.h
MSE_DEPS = $(MSE_SRC) $(MSE_HDRS) $(wildcard c/lib/media/*.c) \
           $(wildcard c/lib/video/*.c) $(wildcard c/lib/audio/*.c)

# --- c/lib/video GREW A DEPENDENCY AND THESE TWO LISTS DID NOT FOLLOW --------
# `c/lib/video/*.c` is a wildcard, and mjpeg.c joined it -- decoding each frame
# through c/lib/image's img_decode() rather than carrying a second baseline JPEG
# decoder. Two things followed and neither was here, so all three host gates
# below broke in two stages (measured 2026-08-28, darwin/arm64):
#
#   the INCLUDE   without -Ic/lib/image these did not fail, they did not
#                 COMPILE: c/lib/video/mjpeg.c:14:10: fatal error: 'img.h' file
#                 not found. test-mse, test-mse-asan and test-mse-negctl were
#                 all red at $(CC), before a single check ran.
#   the LINK      with the include, they failed on _img_decode / _img_free.
#                 img.c registers six decoders, so it pulls the whole image
#                 stack. $(IMG_HOST_SRC) is that set as ONE variable, for the
#                 reason Makefile:3337 gives -- five copies of a decoder list is
#                 five chances for a newly added decoder to be missing from the
#                 test that would have caught its bug.
#
# NOT fixed by dropping mjpeg.c from the wildcard: $(BUILD)/msecheck.elf below
# already links the target spelling of exactly this set ($(IMGCHK_OBJ)
# $(GFX_OBJ) $(RUST_LIB)), and the whole point of having both builds is that
# they are the same sources. A named exception here is also how the drift
# tests/wpt.mk:168 describes started.
#
# Same defect and the same shape as the two .elf files CLAUDE.md records under
# "Two binaries that could not be linked, and `make` said ok".
#
# DEFERRED (=) and not (:=), for the reason tests/canvas.mk:14 spells out:
# IMG_HOST_SRC / IMG_HOST_INC are defined at Makefile:3343 and this fragment is
# -included at :3906. That is the right side of them today; := would capture the
# EMPTY value the day either line moves, and the error would name forty image
# symbols with nothing in it about ordering.
MSE_IMG_SRC = $(IMG_HOST_SRC) $(RUST_LIB_HOST)
MSE_INC     = -Ic/apps/browser -Ic/lib/media -Ic/lib/video -Ic/lib/audio \
              $(IMG_HOST_INC)

# THE TWO-LINE RING-3 ALLOCATOR SHIM, generated rather than committed.
# c/lib/image and c/lib/video declare `void *kmalloc(unsigned long)` as a bare
# extern and rely on the PROGRAM to define it -- preview.c:64, browser_rt.c:44,
# vidcheck.c, demuxcheck.c:38 and c/apps/media/msecheck.c:33 each carry the same
# two lines, and msecheck.c's copy is what makes the .elf below link. Of every
# consumer of these sources, tests/unit/mse_test.c is the one that does not
# carry them, so the host link fails on _kmalloc/_kfree.
#
# It cannot be done with -Dkmalloc=malloc: $(RUST_LIB_HOST) references the
# symbol by NAME from precompiled objects the preprocessor never sees (measured:
# webp::vp8l_pixels, webp::decode_stream, webp::Huffs::new). A real definition
# has to exist.
#
# Generated into $(BUILD) and not committed beside mse_test.c only because the
# line that made this fix owns tests/mse.mk and nothing else. ITS PROPER HOME IS
# tests/unit/mse_test.c, beside the identical two lines msecheck.c already
# carries; whoever next owns that file should move it there and delete this
# rule. Passed as SOURCE, not an object, so the ASan gate instruments it too.
$(BUILD)/mse_kshim.c: tests/mse.mk
	@mkdir -p $(BUILD)
	@printf '%s\n' \
	    '/* Generated by tests/mse.mk -- see the note there. The ring-3 allocator' \
	    ' * shim c/lib/image and c/lib/video expect the PROGRAM to supply. */' \
	    '#include <stdlib.h>' \
	    'void *kmalloc(unsigned long n) { return malloc((size_t)n); }' \
	    'void  kfree(void *p) { free(p); }' > $@

# --- what the BROWSER has to link ------------------------------------------
# The whole point of this feature is that the decoders become reachable from a
# web page, so browser.aex grows the demuxers, both video decoders and the audio
# decoders. They are the same ring-3 objects Preview and /bin/demuxcheck already
# link (VID_OBJ / AUD_OBJ / MED_OBJ), built once and shared -- nothing here is
# a second build of anything.
#
# js_media.c and js_media_src.c need no rule at all: BROWSER_JS_SRC is
# $(wildcard c/apps/browser/js_*.c), so they are picked up with the QuickJS
# flags automatically. That wildcard is why they are named js_*.
MSE_LINK_OBJ := $(MED_OBJ) $(VID_OBJ) $(AUD_OBJ)

# AND THEY GET ONTO THE LINK LINE WITHOUT THE ROOT MAKEFILE BEING TOUCHED.
# browser.elf's recipe names $(BROWSER_OBJ), and a recipe expands when it RUNS
# -- by which time every -include has been read -- so appending here is enough.
# A prerequisite list expands where it is WRITTEN, which is why the dependency
# needs the second, recipe-less rule below (the trick tests/demux.mk uses for
# preview.elf). The whole of this feature therefore costs the shared Makefile
# exactly ONE line: `-include tests/mse.mk`. That is deliberate -- this tree is
# worked on by several lines at once and the Makefile is where they collide.
BROWSER_OBJ  += $(MSE_LINK_OBJ)
$(BUILD)/browser.elf: $(MSE_LINK_OBJ) $(MSE_HDRS)

# --- the fixture -------------------------------------------------------------
# Committed, like every other media fixture here, so the gate means something on
# a machine with no encoder. Regenerate explicitly and expect the numbers to
# move, because a different ffmpeg writes a different file.
mse-fixtures:
	@bash tests/unit/gen_mse.sh $(MSE_FX)

# --- the host gate -----------------------------------------------------------
$(BUILD)/mse_test: tests/unit/mse_test.c $(MSE_DEPS) $(MSE_IMG_SRC) $(BUILD)/mse_kshim.c
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -o $@ tests/unit/mse_test.c \
	    $(BUILD)/mse_kshim.c $(MSE_SRC) c/lib/media/*.c c/lib/video/*.c \
	    c/lib/audio/*.c $(MSE_IMG_SRC) $(MSE_INC) -lm

test-mse: $(BUILD)/mse_test test-mse-nocard-negctl
	@$(BUILD)/mse_test $(MSE_FX)

# Under the sanitizers, because every byte in this path arrived through
# appendBuffer -- i.e. off the network, chosen by a stranger -- and the box walk
# in js_media_src.c is a length field from that stranger driving a cursor.
# -fno-sanitize-recover=all is not optional: without it UBSan prints and carries
# on, the process exits 0, and the run reports clean.
#
# detect_leaks IS PROBED, NOT ASSUMED. It said `detect_leaks=1` unconditionally,
# and on the documented dev host (macOS / Apple Silicon) LeakSanitizer is not in
# the runtime at all: the option is not ignored, it prints "AddressSanitizer:
# detect_leaks is not supported on this platform" and Die()s before main, so
# this gate was `Abort trap: 6` for a reason with nothing to do with a box walk.
# The probe, the note printed when it is off, and the reason it is one probe
# rather than two all live in tests/demux.mk, which is -included immediately
# before this file. (Same option, same day, worse consequence over there: it was
# satisfying test-demux-fuzz-negctl.)
MSE_ASAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
test-mse-asan: $(MSE_DEPS) $(MSE_IMG_SRC) $(BUILD)/mse_kshim.c tests/unit/mse_test.c \
               $(BUILD)/.asan-leaks
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(MSE_ASAN) -w -o $(BUILD)/mse_test_asan tests/unit/mse_test.c \
	    $(BUILD)/mse_kshim.c $(MSE_SRC) c/lib/media/*.c c/lib/video/*.c \
	    c/lib/audio/*.c $(MSE_IMG_SRC) $(MSE_INC) -lm
	@leaks=`cat $(BUILD)/.asan-leaks`; $(ASAN_LEAK_NOTE); \
	 ASAN_OPTIONS=detect_leaks=$$leaks:halt_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/mse_test_asan $(MSE_FX)

# --- THE NEGATIVE CONTROL ----------------------------------------------------
# The bug this whole design exists to prevent, built on purpose:
# -DMSE_CONTROL_CLAIM_AV1 makes isTypeSupported answer yes to av01, which this
# machine cannot decode by any route. REQUIRED TO FAIL, and required to fail in
# the specific way the design predicts -- the AV1 SourceBuffer is accepted, the
# bytes append, and no picture is ever produced. A control that merely failed
# somewhere would prove the suite notices something, not that it notices THIS.
test-mse-negctl: $(MSE_DEPS) $(MSE_IMG_SRC) $(BUILD)/mse_kshim.c \
                 tests/unit/mse_test.c $(MSE_FX)/whole-av1.mp4
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DMSE_CONTROL_CLAIM_AV1=1 -o $(BUILD)/mse_test_neg \
	    tests/unit/mse_test.c $(BUILD)/mse_kshim.c $(MSE_SRC) c/lib/media/*.c \
	    c/lib/video/*.c c/lib/audio/*.c $(MSE_IMG_SRC) $(MSE_INC) -lm
	@if $(BUILD)/mse_test_neg $(MSE_FX) > $(BUILD)/mse_neg.log 2>&1; then \
	    echo "NEGCTL-FAIL: isTypeSupported claimed AV1 and the suite still passed --"; \
	    echo "  which means nothing in it checks the answer against reality."; \
	    exit 1; \
	 elif grep -q 'produced 0 pictures' $(BUILD)/mse_neg.log; then \
	    echo "negctl: claiming AV1 is caught, and caught where it was predicted:"; \
	    grep -m1 'the AV1 stream appended' $(BUILD)/mse_neg.log | sed 's/^/       /'; \
	    grep -m1 'FAIL: a type isTypeSupported said yes to' $(BUILD)/mse_neg.log | sed 's/^/       /'; \
	    grep -c '^FAIL' $(BUILD)/mse_neg.log | sed 's/^/       total failures: /'; \
	 else \
	    echo "NEGCTL-FAIL: the sabotaged build failed, but NOT by failing to decode"; \
	    echo "  the AV1 it claimed -- so this proves the suite is unhappy, not that"; \
	    echo "  it catches a dishonest codec table."; \
	    grep -m5 '^FAIL' $(BUILD)/mse_neg.log | sed 's/^/       /'; exit 1; \
	 fi

# --- THE SECOND NEGATIVE CONTROL: a machine with no sound card ---------------
# -DMSE_CONTROL_NO_CARD_STRAND removes BOTH halves of the "no card means the
# block is delivered vacuously" rule in mel_pump_audio() -- the fresh write and
# the drain of a held tail. It has to remove both: either one alone releases
# the block the other parked, so flipping one is a control that cannot be
# watched failing.
#
# WHY THIS EXISTS AT ALL. The short-write carry-forward fix (which closed
# decoded=60 shown=59 on the device) put an undelivered PCM tail in `apend` and
# refused to decode further until it drained. Both the write and the drain sit
# behind `if (el->snd >= 0 && ...)`, so on a machine with NO card `off` stayed 0,
# the code read that as "nothing was accepted", parked the first block, and
# stopped for ever. js_media.c's os_snd_open returns -1 there and "no card: play
# silent, in time" is an explicitly supported path -- so the fix for a red gate
# would have deadlocked audio on every machine without an audio device.
#
# AND THE CASE HAD TO BE MADE WATCHABLE BEFORE IT COULD BE BELIEVED. The first
# four assertions in test_no_card ALL PASS under the sabotage: video still
# plays, still ends, raises no error, because with no card the clock falls back
# and video never waits on audio. Only `audio_frames_written` distinguishes a
# working silent path (89856, the fixture's whole track) from a stranded pump
# (0). Four green checks over a dead subsystem is this tree's own rule 5, and
# it took four tries to notice -- three earlier "controls" measured a stale
# binary, an inverted guard, and a -D the Makefile never passed on.
#
# Not stranded: test-mse depends on it. tests/audit-stranded.baseline records 61
# controls that are named on a ci-host: line and run never.
test-mse-nocard-negctl: $(MSE_DEPS) $(MSE_IMG_SRC) $(BUILD)/mse_kshim.c \
                        tests/unit/mse_test.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DMSE_CONTROL_NO_CARD_STRAND=1 -o $(BUILD)/mse_test_nocard \
	    tests/unit/mse_test.c $(BUILD)/mse_kshim.c $(MSE_SRC) c/lib/media/*.c \
	    c/lib/video/*.c c/lib/audio/*.c $(MSE_IMG_SRC) $(MSE_INC) -lm
	@if $(BUILD)/mse_test_nocard $(MSE_FX) > $(BUILD)/mse_nocard.log 2>&1; then \
	    echo "NEGCTL-FAIL: the audio pump was sabotaged to strand on a machine with"; \
	    echo "  no sound card and the suite still passed -- so nothing in it checks"; \
	    echo "  that the silent path consumes audio at all."; \
	    exit 1; \
	 elif grep -q 'audio track was still consumed' $(BUILD)/mse_nocard.log; then \
	    echo "negctl: the no-card strand is caught, and caught where predicted:"; \
	    grep -m1 'no-card: audio_frames_written' $(BUILD)/mse_nocard.log | sed 's/^/       /'; \
	    echo "       (and note the four checks ABOVE it still pass -- video plays,"; \
	    echo "        ends and raises no error over a dead audio pump)"; \
	 else \
	    echo "NEGCTL-FAIL: the sabotaged build failed, but NOT on the audio-consumed"; \
	    echo "  assertion -- so this proves the suite is unhappy, not that it catches"; \
	    echo "  a stranded pump."; \
	    grep -m5 '^FAIL' $(BUILD)/mse_nocard.log | sed 's/^/       /'; exit 1; \
	 fi

# --- on the machine ----------------------------------------------------------
# /bin/msecheck runs the same segmented playback on LogitOS, against the same
# fixture, and prints the same numbers -- which is what turns "MSE works" from a
# claim about a clang build on Linux into a claim about the machine: mini-libc's
# arena allocator, -ffreestanding -msse2, a 32 KiB stack, and a real sound card.
$(BUILD)/mseobj/%.o: %.c $(MSE_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

MSECHK_OBJ := $(BUILD)/mseobj/c/apps/browser/js_media_src.o \
              $(BUILD)/mseobj/c/apps/media/msecheck.o

$(BUILD)/msecheck.elf: $(MSECHK_OBJ) $(MED_OBJ) $(VID_OBJ) $(AUD_OBJ) \
                       $(IMGCHK_OBJ) $(GFX_OBJ) $(RUST_LIB) $(LIBM_OBJ) \
                       $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/msecheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/msecheck.crt0c.o $(MSECHK_OBJ) $(MED_OBJ) $(VID_OBJ) \
	    $(AUD_OBJ) $(IMGCHK_OBJ) $(GFX_OBJ) $(RUST_LIB) $(LIBM_OBJ) \
	    $(LIBC_OBJS) --end-group
$(BUILD)/msecheck.aex: $(BUILD)/msecheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/msecheck.elf $@ msecheck - 'M' 120 170 220

# On to the disk WITHOUT editing the root Makefile's $(DISK) recipe: FS_FILES is
# expanded when that recipe runs, after every -include has been read.
MSE_DISK_SEGS := $(MSE_FX)/init-video.mp4:/media/mse/init-video.mp4 \
                 $(MSE_FX)/init-audio.mp4:/media/mse/init-audio.mp4 \
                 $(MSE_FX)/video-1.m4s:/media/mse/video-1.m4s \
                 $(MSE_FX)/video-2.m4s:/media/mse/video-2.m4s \
                 $(MSE_FX)/video-3.m4s:/media/mse/video-3.m4s \
                 $(MSE_FX)/video-4.m4s:/media/mse/video-4.m4s \
                 $(MSE_FX)/audio-1.m4s:/media/mse/audio-1.m4s \
                 $(MSE_FX)/audio-2.m4s:/media/mse/audio-2.m4s \
                 $(MSE_FX)/audio-3.m4s:/media/mse/audio-3.m4s \
                 $(MSE_FX)/audio-4.m4s:/media/mse/audio-4.m4s \
                 $(MSE_FX)/audio-5.m4s:/media/mse/audio-5.m4s
FS_FILES += $(BUILD)/msecheck.aex:/bin/msecheck $(MSE_DISK_SEGS)
$(DISK): $(BUILD)/msecheck.aex $(MSE_FX)/init-video.mp4

test-mse-os: $(ISO) $(DISK)
	@bash tests/boot/run-mse-test.sh $(ISO) $(DISK)

# --- and the one neither of the two above can make ---------------------------
# test-mse drives the engine on the host; test-mse-os drives it on the machine
# through /bin/msecheck. NEITHER GOES THROUGH THE BROWSER: no QuickJS, no DOM,
# no <video> element, no fetch, no layout, no compositor. Both can be green
# while a page's <video> does nothing, and that is the state this line found.
#
# This one loads a page over HTTP in browser.aex and reads the element's own
# framesShown counter -- twice, seconds apart, because one picture and a freeze
# satisfies "> 0" -- and then reads the WAV file QEMU's card wrote, because
# hda.c's DMA engine runs happily into silence with every register correct.
test-video-page: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_video_page.py $(ISO) $(DISK)
