# tests/demux.mk -- the container demuxers: what builds them and what gates them.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 2600-line file is where their edits collide. The
# root Makefile carries one `-include tests/demux.mk` line, plus the two edits
# that cannot live here -- c/lib/media added to C_SRC's filter-out (so the
# demuxers stay OUT of the kernel) and $(MED_OBJ) in Preview's link. That is
# the same arrangement tests/h265.mk, tests/nic.mk and tests/audio.mk use.

.PHONY: test-demux test-demux-units test-demux-diff test-demux-lacing \
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
	$(CC) $(UCFLAGS) -c $< -o $@

# Preview links the demuxers, both video decoders and the audio decoders. The
# root Makefile's recipe names $(MED_OBJ) -- a recipe expands when it runs, by
# which time every -include has been read -- but a PREREQUISITE list expands
# where it is written, so the dependency goes here on a second, recipe-less
# rule for the same target. Same trick tests/h265.mk uses for $(DISK).
$(BUILD)/preview.elf: $(MED_OBJ) $(AUD_OBJ) $(MED_HDRS)

# /bin/demuxcheck -- opens a container on the device and prints the same digest
# the host build prints, which is what turns "it also works on LogitOS" into a
# comparison rather than a claim. ONE source file, built twice; see its header.
$(BUILD)/medobj/c/apps/media/demuxcheck.o: c/apps/media/demuxcheck.c $(MED_HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(UCFLAGS) -c $< -o $@

$(BUILD)/demuxcheck.elf: $(BUILD)/medobj/c/apps/media/demuxcheck.o $(MED_OBJ) \
                         $(VID_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(GFX_OBJ) \
                         $(RUST_LIB) $(LIBM_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/demuxcheck.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/demuxcheck.crt0c.o $(BUILD)/medobj/c/apps/media/demuxcheck.o \
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
DEMUXCHECK_DEPS = c/apps/media/demuxcheck.c $(MED_SRC) $(wildcard c/lib/video/*.c) \
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

# --- the parser's own unit assertions ----------------------------------------
# A whole-file test says something is wrong; a unit test says what. These run
# the bounded reader, the tick arithmetic, the sniffer and the Annex B rewrite
# against hand-written cases whose right answer is known by construction.
test-demux-units: $(BUILD)/demux_test
	@$(BUILD)/demux_test units

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
test-demux-diff: $(BUILD)/demux_test
	@if ! command -v ffprobe >/dev/null 2>&1; then \
	    echo "test-demux-diff: no ffmpeg on this machine -- differential skipped"; \
	 else \
	    python3 tests/unit/demux_diff.py $(BUILD)/demux_test $(BUILD) $(DIFF_FILES); \
	 fi

# --- lacing, against arithmetic rather than another implementation -----------
# ffmpeg's Matroska muxer never writes lacing, so the laced fixtures are built
# by tests/unit/gen_laced.py and their frame boundaries are known BY
# CONSTRUCTION. Needs no ffmpeg at all.
test-demux-lacing: $(BUILD)/demux_test
	@python3 tests/unit/demux_lacing.py $(BUILD)/demux_test $(BUILD) $(MEDIA_FX)

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

test-demux-fuzz: $(BUILD)/.asan-leaks
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(DEMUX_ASAN_FLAGS) -w $(MED_INC) \
	    -o $(BUILD)/demux_fuzz tests/unit/demux_fuzz.c $(MED_SRC)
	@leaks=`cat $(BUILD)/.asan-leaks`; $(ASAN_LEAK_NOTE); \
	 $(DEMUX_ASAN_ENV) $(BUILD)/demux_fuzz $(SCALE) $(SEED) $(MEDIA_FX)

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

# NEGATIVE CONTROL FOR THE FUZZER ITSELF. A fuzz target that has never caught
# anything is indistinguishable from one wired to /dev/null, and this one
# reports "0 failures" in its own output -- so the sanitizers have to be shown
# to be live. -DDEMUX_FUZZ_SABOTAGE takes a length-prefixed NAL's length at
# face value in media_to_annexb: a heap over-read driven directly by four
# attacker-chosen bytes, which is the exact bug class this fuzzer exists for.
# REQUIRED TO FAIL, and required to fail with an ASan report specifically.
#
# THE GUARD USED TO BE `grep -q 'AddressSanitizer'` AND THAT IS HOW IT REPORTED
# A FALSE GREEN. On darwin/arm64 the `detect_leaks=1` above made the runtime
# print "AddressSanitizer: detect_leaks is not supported on this platform" and
# abort before main -- a non-zero exit whose message contains the word, so the
# elif matched and this control announced a caught over-read on a run that never
# executed one byte of media_to_annexb. The option is probed now, and the guard
# is tightened to the string ASan prints for an actual FINDING ('ERROR:
# AddressSanitizer'), with the runtime-refused case broken out and named -- so
# the next option this runtime dislikes fails loudly here instead of passing.
test-demux-fuzz-negctl: $(BUILD)/.asan-leaks
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g $(DEMUX_ASAN_FLAGS) -w -DDEMUX_FUZZ_SABOTAGE=1 $(MED_INC) \
	    -o $(BUILD)/demux_fuzz_neg tests/unit/demux_fuzz.c $(MED_SRC)
	@leaks=`cat $(BUILD)/.asan-leaks`; $(ASAN_LEAK_NOTE); \
	 if $(DEMUX_ASAN_ENV) $(BUILD)/demux_fuzz_neg 3 $(SEED) $(MEDIA_FX) \
	        >$(BUILD)/demux_fuzz_neg.log 2>&1; then \
	    echo "NEGCTL-FAIL: a deliberate out-of-bounds read in the NAL length walk"; \
	    echo "  did not trip the fuzzer -- the sanitizers are not doing anything."; \
	    exit 1; \
	 elif grep -q 'is not supported on this platform' $(BUILD)/demux_fuzz_neg.log; then \
	    echo "NEGCTL-FAIL: the ASan RUNTIME refused one of its own options and died"; \
	    echo "  before main, so nothing was measured. This is the exact false green"; \
	    echo "  this control carried until 2026-08-28 -- the refusal message contains"; \
	    echo "  the word AddressSanitizer, and a loose grep read it as a finding."; \
	    head -3 $(BUILD)/demux_fuzz_neg.log | sed 's/^/       /'; exit 1; \
	 elif grep -q 'ERROR: AddressSanitizer' $(BUILD)/demux_fuzz_neg.log; then \
	    echo "negctl: the injected NAL-length over-read is caught by AddressSanitizer"; \
	    grep -m1 'ERROR: AddressSanitizer' $(BUILD)/demux_fuzz_neg.log | sed 's/^/       /'; \
	 else \
	    echo "NEGCTL-FAIL: the sabotaged build failed, but not with an ASan report --"; \
	    echo "  so this proves the harness noticed, not that the sanitizer did."; \
	    head -20 $(BUILD)/demux_fuzz_neg.log | sed 's/^/       /'; exit 1; \
	 fi

# --- NEGATIVE CONTROLS FOR THE GATES -----------------------------------------
# A gate that never fails is not a gate. Two deliberate faults, each a real bug
# class, each behind a -DDEMUX_CONTROL_* that no shipping build defines:
#
#   NO_CTTS     composition offsets ignored, so presentation time is taken to
#               equal decode time. Invisible on every stream without B frames,
#               and on a stream with them it plays the pictures at the wrong
#               times. The differential must reject it.
#   NO_LACING   every Matroska block treated as holding one frame. This does
#               not fail loudly: audio still comes out, with eleven frames
#               glued to the end of every twelfth. The lacing check must
#               reject it.
test-demux-negctl:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -w -DDEMUX_CONTROL_NO_CTTS=1 $(MED_INC) \
	    -o $(BUILD)/demux_test_noctts tests/unit/demux_test.c $(MED_SRC)
	@if ! command -v ffprobe >/dev/null 2>&1; then \
	    echo "test-demux-negctl: no ffmpeg -- the ctts control needs the differential"; \
	 elif python3 tests/unit/demux_diff.py $(BUILD)/demux_test_noctts $(BUILD) \
	        $(MEDIA_FX)/h264-mp3.mp4 $(MEDIA_FX)/frag.mp4 \
	        >$(BUILD)/demux_noctts.log 2>&1; then \
	    echo "NEGCTL-FAIL: the ffmpeg differential passed a demuxer that ignores"; \
	    echo "  composition offsets, so it is not measuring timestamps."; exit 1; \
	 else \
	    echo "negctl: ignoring ctts is rejected by the differential"; \
	    grep -m2 'sample .* pts' $(BUILD)/demux_noctts.log | sed 's/^/       /'; \
	 fi
	@$(CC) -O2 -w -DDEMUX_CONTROL_NO_LACING=1 $(MED_INC) \
	    -o $(BUILD)/demux_test_nolace tests/unit/demux_test.c $(MED_SRC)
	@if python3 tests/unit/demux_lacing.py $(BUILD)/demux_test_nolace $(BUILD) \
	        $(MEDIA_FX) >$(BUILD)/demux_nolace.log 2>&1; then \
	    echo "NEGCTL-FAIL: the lacing check passed a demuxer with lacing removed."; \
	    exit 1; \
	 else \
	    echo "negctl: removing Matroska lacing is rejected"; \
	    grep -m2 'frames, expected\|size .*expected\|payload is' $(BUILD)/demux_nolace.log \
	        | sed 's/^/       /'; \
	 fi

# --- A/V SYNCHRONISATION, MEASURED -------------------------------------------
# "It stays in sync" is not checkable by watching, and on this machine the
# interesting case -- decode falls behind -- is the normal case. avclock takes
# `now_ns` as an argument so a whole minute of playback can be simulated to the
# nanosecond at four decode speeds. Reports drift; gates on it; and includes a
# control that removes the bounded-audio-lead rule and must be shown to drift.
test-avsync: $(BUILD)/avsync_test
	@$(BUILD)/avsync_test

# --- the digest the guest must reproduce -------------------------------------
# Pinned from the HOST build of demuxcheck, so tests/boot/run-demux-test.sh has
# something to compare with. Re-pin after regenerating the fixtures.
test-demux-expect: $(BUILD)/demuxcheck_host
	@{ $(BUILD)/demuxcheck_host -decode $(MEDIA_FX)/h264-mp3-nobf.mp4; \
	   $(BUILD)/demuxcheck_host -decode $(MEDIA_FX)/h264-flac.mkv; \
	   $(BUILD)/demuxcheck_host $(MEDIA_FX)/frag.mp4; \
	   $(BUILD)/demuxcheck_host $(MEDIA_FX)/h264-mp3.mp4; \
	   $(BUILD)/demuxcheck_host $(MEDIA_FX)/laced-xiph.mkv; \
	   $(BUILD)/demuxcheck_host $(MEDIA_FX)/vp9-opus.webm; } \
	 | grep -E '^(MEDIA |MEDIA-|TRACK |ORDER |DEMUX-CRC)' > $(MEDIA_FX)/expected-guest.txt
	@echo "pinned $$(wc -l < $(MEDIA_FX)/expected-guest.txt) lines to $(MEDIA_FX)/expected-guest.txt"

# --- ON THE DEVICE -----------------------------------------------------------
# Everything above is a glibc build on Linux. Only this says the demuxers work
# on LogitOS, where the allocator is mini-libc's arena and its realloc cannot
# always grow in place -- which is precisely what building a sample index does
# thousands of times.
test-demux-os: $(ISO) $(DISK)
	@bash tests/boot/run-demux-test.sh $(ISO) $(DISK) $(MEDIA_FX)/expected-guest.txt

# Everything host-side, in one target.
test-demux: test-demux-units test-demux-lacing test-demux-diff test-avsync \
            test-demux-fuzz test-demux-fuzz-negctl test-demux-negctl
	@echo "test-demux: units, lacing, the ffmpeg differential, A/V sync, the"
	@echo "            fuzzer and all three negative controls"
