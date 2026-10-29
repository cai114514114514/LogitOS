# tests/demux.mk -- the container demuxers: what builds them and what gates them.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 2600-line file is where their edits collide. The
# root Makefile carries one `-include tests/demux.mk` line, plus the two edits
# that cannot live here -- c/lib/media added to C_SRC's filter-out (so the
# demuxers stay OUT of the kernel) and $(MED_OBJ) in Preview's link. That is
# the same arrangement tests/h265.mk, tests/nic.mk and tests/audio.mk use.

.PHONY: \
        test-demux-fuzz test-demux-fuzz-deep test-demux-fuzz-negctl \
        test-demux-negctl test-avsync test-demux-os test-demux-expect \
        media-fixtures

MED_SRC  := $(wildcard c/lib/media/*.c)
MED_HDRS := $(wildcard c/lib/media/*.h)
MED_INC  := -Ic/lib/media
MEDIA_FX := tests/fixtures/media

# --- built for the TARGET, ring 3 --------------------------------------------
# Same shape and the same reasoning as VID_OBJ and AUD_OBJ in the root Makefile:
# c/lib/media is filtered out of the kernel's C_SRC on purpose and compiled here
# with the userland flags against mini-libc. Consumers are Preview and
# /bin/demuxcheck.
MED_OBJ := $(patsubst %.c,$(BUILD)/medobj/%.o,$(MED_SRC))

$(BUILD)/medobj/%.o: %.c $(MED_HDRS)
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@

# Preview links the demuxers, both video decoders and the audio decoders. The
# root Makefile's recipe names $(MED_OBJ) -- a recipe expands when it runs, by
# which time every -include has been read -- but a PREREQUISITE list expands
# where it is written, so the dependency goes here on a second, recipe-less
# rule for the same target. Same trick tests/h265.mk uses for $(DISK).
$(BUILD)/preview.elf: $(MED_OBJ) $(AUD_OBJ) $(MED_HDRS)

# /bin/demuxcheck -- opens a container on the device and prints the same digest
# the host build prints, which is what turns "it also works on LogitOS" into a
# comparison rather than a claim. ONE source file, built twice; see its header.
$(BUILD)/medobj/tests/guest/media/demuxcheck.o: tests/guest/media/demuxcheck.c $(MED_HDRS)
	@mkdir -p $(dir $@)
	$(COMPILE) $(UCFLAGS) -c $< -o $@

$(BUILD)/demuxcheck.elf: $(BUILD)/medobj/tests/guest/media/demuxcheck.o $(MED_OBJ) \
                         $(VID_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(GFX_OBJ) \
                         $(RUST_LIB) $(LIBM_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/demuxcheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/demuxcheck.crt0c.o $(BUILD)/medobj/tests/guest/media/demuxcheck.o \
	    $(MED_OBJ) $(VID_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(GFX_OBJ) \
	    $(RUST_LIB) $(LIBM_OBJ) $(LIBC_OBJS) --end-group
$(BUILD)/demuxcheck.aex: $(BUILD)/demuxcheck.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/demuxcheck.elf $@ demuxcheck - 'M' 150 150 150

# On to the disk image WITHOUT editing the root Makefile's $(DISK) recipe:
# FS_FILES is expanded when that recipe runs, which is after every -include has
# been read, and mkfs.py takes host[:/dest] for any argument.
#
# Six containers, chosen to be six different code paths and not six files:
# progressive MP4 (decodable), Matroska with FLAC (decodable), fragmented MP4
# (no sample table at all), an MP4 with B frames (non-zero ctts), a laced
# Matroska (three frames per block), and a WebM whose codecs this system cannot
# decode -- which must open and SAY so.
FS_FILES += $(BUILD)/demuxcheck.aex:/bin/demuxcheck \
            $(MEDIA_FX)/h264-mp3-nobf.mp4:/media/clip.mp4 \
            $(MEDIA_FX)/h264-flac.mkv:/media/clip.mkv \
            $(MEDIA_FX)/frag.mp4:/media/clip-frag.mp4 \
            $(MEDIA_FX)/h264-mp3.mp4:/media/clip-bframes.mp4 \
            $(MEDIA_FX)/laced-xiph.mkv:/media/clip-laced.mkv \
            $(MEDIA_FX)/vp9-opus.webm:/media/clip.webm
$(DISK): $(BUILD)/demuxcheck.aex $(MEDIA_FX)/h264-mp3-nobf.mp4 \
         $(MEDIA_FX)/h264-flac.mkv $(MEDIA_FX)/frag.mp4 \
         $(MEDIA_FX)/h264-mp3.mp4 $(MEDIA_FX)/laced-xiph.mkv \
         $(MEDIA_FX)/vp9-opus.webm

# --- host tools --------------------------------------------------------------
$(BUILD)/demux_test: tests/unit/demux_test.c $(MED_SRC) $(MED_HDRS)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -g -Wall -Wextra -o $@ tests/unit/demux_test.c $(MED_SRC) $(MED_INC)

$(BUILD)/avsync_test: tests/unit/avsync_test.c c/lib/media/avclock.c $(MED_HDRS)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/avsync_test.c c/lib/media/avclock.c $(MED_INC)

# The host build of the SAME program that ships as /bin/demuxcheck.
#
# A WILDCARD GREW A DEPENDENCY AND THIS LINE DID NOT FOLLOW. `c/lib/video/*.c`
# is a wildcard, and mjpeg.c joined it -- decoding each frame through
# c/lib/image's img_decode() rather than carrying a second baseline JPEG
# decoder, which is the right trade and the reason the dependency exists at
# all. Two things followed from that and neither was here, so this rule broke
# in two stages (both measured 2026-08-28, darwin/arm64):
#
#   the INCLUDE   without -Ic/lib/image this did not fail, it did not COMPILE:
#                 c/lib/video/mjpeg.c:14:10: fatal error: 'img.h' file not found
#   the LINK      with the include, it failed on _img_decode / _img_free.
#                 img.c registers six decoders, so it pulls the whole image
#                 stack; $(IMG_HOST_SRC) is that set as ONE variable for the
#                 reason Makefile:3337 gives -- five copies of a decoder list is
#                 five chances for a newly added decoder to be missing from the
#                 test that would have caught its bug.
#
# NOT fixed by dropping mjpeg.c from the wildcard. $(BUILD)/demuxcheck.elf
# above already links the target spelling of exactly this set ($(IMGCHK_OBJ)
# $(GFX_OBJ) $(RUST_LIB)), and this file's own header says "ONE source file,
# built twice". A host build that omits a TU the guest links pins
# test-demux-expect's digest from a program that is not the one on the disk.
# demuxcheck.c:38 already carries the two-line kmalloc/kfree shim c/lib/image
# needs in ring 3; it is what makes this link close with nothing else added.
#
# Same defect and the same shape as the two .elf files CLAUDE.md records under
# "Two binaries that could not be linked, and `make` said ok" -- there it was
# $(VID_OBJ)'s consumers, here it is the host build of the same sources, and it
# survived for the mirror-image reason: nobody deleted the binary, so nothing
# asked for it back.
#
# DEFERRED (=) and not (:=), for the reason tests/canvas.mk:14 spells out:
# IMG_HOST_SRC / IMG_HOST_INC are defined at Makefile:3343 and this fragment is
# -included at :3905. That is the right side of them today; := would capture
# the EMPTY value the day either line moves, and the error would name forty
# image symbols with nothing in it about ordering.
DEMUXCHECK_DEPS = tests/guest/media/demuxcheck.c $(MED_SRC) $(wildcard c/lib/video/*.c) \
                  $(wildcard c/lib/audio/*.c) $(IMG_HOST_SRC)
$(BUILD)/demuxcheck_host: $(DEMUXCHECK_DEPS) $(MED_HDRS) $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -o $@ $(DEMUXCHECK_DEPS) $(RUST_LIB_HOST) \
	    $(MED_INC) -Ic/lib/video -Ic/lib/audio $(IMG_HOST_INC) -lm

# --- the fixtures ------------------------------------------------------------
# Committed, for the same reason tests/fixtures/video/sample.h264 is: the gates
# have to mean something on a machine with no encoder installed. Regenerate
# explicitly with `make media-fixtures` and expect every pinned digest to move.
media-fixtures:
	@bash tests/unit/gen_media.sh $(MEDIA_FX)
	@echo "regenerated -- now run 'make test-demux-expect' to re-pin the guest digest"


# --- THE DIFFERENTIAL AGAINST FFMPEG -----------------------------------------
# The bar next door is bit-exactness against ffmpeg's decoders. The equivalent
# for a demuxer is the same sample boundaries, the same timestamps, the same
# keyframe flags, the same codec-configuration bytes and the same elementary
# stream out -- all five checkable byte for byte. See the header of
# tests/unit/demux_diff.py for the two places ffmpeg is asked a question rather
# than trusted blindly, and why.
#
# Needs ffmpeg. Without it the target says so and passes, exactly as
# tools/genvideo265.sh does for libx265 -- but the committed fixtures are still
# demuxed by test-demux-units and test-demux-lacing, which need nothing.
DIFF_FILES := $(MEDIA_FX)/h264-mp3.mp4 $(MEDIA_FX)/h264-mp3-nobf.mp4 \
              $(MEDIA_FX)/frag.mp4 $(MEDIA_FX)/frag-everyframe.mp4 \
              $(MEDIA_FX)/h265.mp4 $(MEDIA_FX)/aac.m4a $(MEDIA_FX)/pcm.mov \
              $(MEDIA_FX)/h264-mp3.mkv $(MEDIA_FX)/h264-flac.mkv \
              $(MEDIA_FX)/vp9-opus.webm $(MEDIA_FX)/mp3.mka \
              $(MEDIA_FX)/laced-xiph.mkv $(MEDIA_FX)/laced-fixed.mkv \
              $(MEDIA_FX)/laced-ebml.mkv $(MEDIA_FX)/laced-none.mkv


# --- FUZZ, under ASan + UBSan ------------------------------------------------
# A container comes off the network and is the most attacker-shaped input in
# this system. -fno-sanitize-recover=all is not optional: without it UBSan
# PRINTS a diagnostic and carries on, the process exits 0, and the run reports
# clean while undefined behaviour scrolls past. (That is not hypothetical --
# it is exactly what the audio line found in its own harness.)
DEMUX_ASAN_FLAGS := -fsanitize=address,undefined -fno-sanitize-recover=all \
                    -fno-omit-frame-pointer

# --- DOES THIS HOST'S ASan HAVE A LEAK DETECTOR? -----------------------------
# This said `detect_leaks=1` unconditionally, and on the documented dev host
# (macOS / Apple Silicon) LeakSanitizer is not built into the runtime at all.
# The option is not ignored -- the runtime prints
#
#     ==23580==AddressSanitizer: detect_leaks is not supported on this platform.
#
# and Die()s BEFORE main. Two consequences, and the second is much worse than
# the first (both measured 2026-08-28, darwin/arm64, Xcode clang):
#
#   test-demux-fuzz         `make: *** [test-demux-fuzz] Abort trap: 6`. Red for
#                           a reason that has nothing to do with a container
#                           parser -- not one iteration ran.
#
#   test-demux-fuzz-negctl  printed `negctl: the injected NAL-length over-read
#                           is caught by AddressSanitizer` AND EXITED 0. The
#                           sabotaged build "failed" (exit 134, SIGABRT) and the
#                           guard was `grep -q 'AddressSanitizer'`, which the
#                           refusal message contains. The one control that
#                           exists to prove the sanitizer is live was being
#                           satisfied by the sanitizer refusing to start, on a
#                           run that never reached the injected bug.
#
# So the option is PROBED, not assumed: LSan's presence is a property of the
# runtime that actually got linked, not of `uname` -- it has moved between
# toolchain versions on Linux too. Cached in a file so the compile+run happens
# once per $(BUILD) rather than once per reference. Written here and used by
# tests/mse.mk as well, which is -included immediately after this file; one
# probe, because two probes of one host property is two chances to disagree.
$(BUILD)/.asan-leaks: tests/demux.mk
	@mkdir -p $(BUILD)
	@printf 'int main(void){return 0;}\n' > $(BUILD)/.asan-leaks.c
	@# `fi 2>/dev/null`, and it has to be on the COMPOUND. The probe dies from
	@# SIGABRT where LSan is absent, and the shell reports a signalled child
	@# ("Abort trap: 6") on its own stderr rather than the child's -- so
	@# redirecting the child, or wrapping it in ( ), suppresses nothing (both
	@# tried). Without this the noise lands in every gate's log looking exactly
	@# like the crash this rule exists to prevent.
	@if $(CC) -fsanitize=address -o $(BUILD)/.asan-leaks.probe \
	        $(BUILD)/.asan-leaks.c >/dev/null 2>&1 && \
	    ASAN_OPTIONS=detect_leaks=1 $(BUILD)/.asan-leaks.probe >/dev/null 2>&1; then \
	     echo 1 > $@; \
	 else \
	     echo 0 > $@; \
	 fi 2>/dev/null

# Deferred (=): `$$leaks` is a SHELL variable each recipe sets from the stamp.
DEMUX_ASAN_ENV = ASAN_OPTIONS=detect_leaks=$$leaks:halt_on_error=1 \
                 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1

# Printed on EVERY run where the detector is off, not once when the probe ran.
# A clean ASan run without a leak detector is exactly as green as one with it,
# and the difference has to be visible in the log a person actually reads --
# otherwise "the parsers are fuzzed under ASan" quietly stops covering leaks
# and nothing in the output ever says so.
ASAN_LEAK_NOTE = if [ "$$leaks" = 0 ]; then \
	    echo "  [asan] LEAK DETECTION OFF -- this host's AddressSanitizer runtime has"; \
	    echo "         no LeakSanitizer (darwin/arm64 ships none; detect_leaks=1 makes"; \
	    echo "         it Die() before main). Memory SAFETY is still checked below;"; \
	    echo "         leaks are NOT. Settle it on any host with:"; \
	    echo "           printf 'int main(void){return 0;}' >/tmp/p.c &&"; \
	    echo "           $(CC) -fsanitize=address -o /tmp/p /tmp/p.c &&"; \
	    echo "           ASAN_OPTIONS=detect_leaks=1 /tmp/p"; \
	 fi

SCALE ?= 8
SEED  ?= 0x243F6A8885A308D3


# The soak. Not part of test-demux -- it takes minutes -- but it is what a
# "the parsers are fuzzed" claim should be able to point at.
#   make test-demux-fuzz-deep SCALE=60
test-demux-fuzz-deep: $(BUILD)/.asan-leaks
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(DEMUX_ASAN_FLAGS) -w $(MED_INC) \
	    -o $(BUILD)/demux_fuzz tests/unit/demux_fuzz.c $(MED_SRC)
	@leaks=`cat $(BUILD)/.asan-leaks`; $(ASAN_LEAK_NOTE); \
	 for s in 0x243F6A8885A308D3 0x13198A2E03707344 0xA4093822299F31D0 \
	          0x082EFA98EC4E6C89 0x452821E638D01377; do \
	    echo "--- seed $$s ---"; \
	    $(DEMUX_ASAN_ENV) $(BUILD)/demux_fuzz $(SCALE) $$s $(MEDIA_FX) || exit 1; \
	 done






