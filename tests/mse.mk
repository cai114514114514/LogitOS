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

.PHONY: test-mse test-mse-asan test-mse-negctl test-mse-os mse-fixtures

MSE_FX   := tests/fixtures/mse
MSE_SRC  := c/apps/browser/js_media_src.c
MSE_HDRS := c/apps/browser/js_media.h
MSE_DEPS := $(MSE_SRC) $(MSE_HDRS) $(wildcard c/lib/media/*.c) \
            $(wildcard c/lib/video/*.c) $(wildcard c/lib/audio/*.c)
MSE_INC  := -Ic/apps/browser -Ic/lib/media -Ic/lib/video -Ic/lib/audio

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
$(BUILD)/mse_test: tests/unit/mse_test.c $(MSE_DEPS)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -Wno-unused-parameter -o $@ tests/unit/mse_test.c \
	    $(MSE_SRC) c/lib/media/*.c c/lib/video/*.c c/lib/audio/*.c $(MSE_INC) -lm

test-mse: $(BUILD)/mse_test
	@$(BUILD)/mse_test $(MSE_FX)

# Under the sanitizers, because every byte in this path arrived through
# appendBuffer -- i.e. off the network, chosen by a stranger -- and the box walk
# in js_media_src.c is a length field from that stranger driving a cursor.
# -fno-sanitize-recover=all is not optional: without it UBSan prints and carries
# on, the process exits 0, and the run reports clean.
MSE_ASAN := -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
test-mse-asan: $(MSE_DEPS) tests/unit/mse_test.c
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(MSE_ASAN) -w -o $(BUILD)/mse_test_asan tests/unit/mse_test.c \
	    $(MSE_SRC) c/lib/media/*.c c/lib/video/*.c c/lib/audio/*.c $(MSE_INC) -lm
	@ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 $(BUILD)/mse_test_asan $(MSE_FX)

# --- THE NEGATIVE CONTROL ----------------------------------------------------
# The bug this whole design exists to prevent, built on purpose:
# -DMSE_CONTROL_CLAIM_AV1 makes isTypeSupported answer yes to av01, which this
# machine cannot decode by any route. REQUIRED TO FAIL, and required to fail in
# the specific way the design predicts -- the AV1 SourceBuffer is accepted, the
# bytes append, and no picture is ever produced. A control that merely failed
# somewhere would prove the suite notices something, not that it notices THIS.
test-mse-negctl: $(MSE_DEPS) tests/unit/mse_test.c $(MSE_FX)/whole-av1.mp4
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -w -DMSE_CONTROL_CLAIM_AV1=1 -o $(BUILD)/mse_test_neg \
	    tests/unit/mse_test.c $(MSE_SRC) c/lib/media/*.c c/lib/video/*.c \
	    c/lib/audio/*.c $(MSE_INC) -lm
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
