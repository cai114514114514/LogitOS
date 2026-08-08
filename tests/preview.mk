# Preview test targets (test-preview, test-preview-timing, test-preview-negctl)
# and the fixtures + measurement binaries Preview needs that nothing else puts
# on the disk.
#
# Own fragment for the same reason as tests/demux.mk and tests/h265.mk: this
# tree is worked on by several lines at once and the root Makefile -- and in
# particular its $(DISK) recipe -- is a file they all touch. Appending to
# FS_FILES works because that recipe expands when it RUNS, which is after every
# -include has been read; the prerequisites go on a second, recipe-less rule
# for the same target.

# aac.m4a is AAC-in-MP4 (the pair every phone and every MP4 on the web
# actually carries) and pcm.mov is BIG-ENDIAN PCM in ISO-BMFF -- the two audio
# framings Preview repacks rather than decoding a second way. Neither was on
# the disk, so neither was reachable from the machine.
PREVIEW_FX := tests/fixtures/media/aac.m4a tests/fixtures/media/pcm.mov \
              tests/fixtures/media/h265.mp4 tests/fixtures/media/mp3.mka
PREVIEW_AS := $(sort $(wildcard tests/fixtures/preview/*.as))
FS_FILES += tests/fixtures/media/aac.m4a:/media/aac.m4a \
            tests/fixtures/media/pcm.mov:/media/pcm.mov \
            tests/fixtures/media/h265.mp4:/media/clip-h265.mp4 \
            tests/fixtures/media/mp3.mka:/media/clip-audio.mka \
            $(foreach s,$(PREVIEW_AS),$(s):/usr/as/$(notdir $(s))) \
            $(BUILD)/previewplay.aex:/bin/previewplay \
            $(BUILD)/previewnegctl.aex:/bin/previewnegctl
$(DISK): $(PREVIEW_FX) $(PREVIEW_AS) $(BUILD)/previewplay.aex $(BUILD)/previewnegctl.aex

# --- the reference decode the screen is compared against --------------------
# See the header of tests/unit/img_dump.c for why the ANIMATED cases cannot use
# PIL as their reference and the still ones can. The dump is the host build of
# the identical c/lib/image sources the guest runs, which is the same
# host-vs-guest comparison make test-imgcheck makes -- what pins those decoders
# against an independent implementation is make test-img-anim / test-img-still.
PREVIEW_IMG_FX := $(BUILD)/dot.png tests/fixtures/image/still.bmp \
                  tests/fixtures/image/still.webp tests/fixtures/image/icon.ico \
                  tests/fixtures/image/rot.jpg tests/fixtures/image/anim.gif \
                  tests/fixtures/image/anim.apng

$(BUILD)/img_dump: tests/unit/img_dump.c $(IMG_HOST_SRC) $(RUST_LIB_HOST)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -o $@ tests/unit/img_dump.c $(IMG_HOST_SRC) $(RUST_LIB_HOST) $(IMG_HOST_INC)

$(BUILD)/previewref/.stamp: $(BUILD)/img_dump $(PREVIEW_IMG_FX)
	@rm -rf $(BUILD)/previewref && mkdir -p $(BUILD)/previewref
	@$(foreach f,$(PREVIEW_IMG_FX),$(BUILD)/img_dump $(f) $(BUILD)/previewref $(notdir $(f)) &&) true
	@touch $@

# --- the pixel gate ---------------------------------------------------------
# Boots the desktop, opens Preview from the Dock and walks every format it can
# decode, comparing the SCREEN against a host decode of the same fixture bytes.
# See the header of tests/qmp/qmp_preview.py for what each case asserts.
test-preview: $(ISO) $(DISK) $(BUILD)/previewref/.stamp
	@python3 tests/qmp/qmp_preview.py --iso $(ISO) --disk $(DISK) \
	    --ref $(BUILD)/previewref

# --- the timing gate, and its negative control ------------------------------
# THE SAME PLAYER, ENTERED FROM THE SHELL. PREVIEW_CLI builds c/apps/gui/
# preview.c with a main() instead of an app_main(): one file from argv, one
# animation loop, then exit. Nothing else about the player changes -- it opens
# the same window through the same syscalls and runs the same loop -- so the
# number it prints is a number about the shipped code, which a separately
# written timing harness would not be. Driving it over the serial console
# rather than over QMP makes the measurement deterministic: no pointer, no
# screendump latency, no dock geometry.
#
# PREVIEW_NO_ANIM_TIMING is the NEGATIVE CONTROL, and it removes exactly one
# thing: the wait that holds a frame until its declared delay has elapsed. The
# frames are still decoded and their delays are still read and still printed --
# so a decoder-level test of the delays still passes, which is the point. If
# the timing assertion can still pass against that build, it is not measuring
# the player and the 1490 ms it reports is a number nobody checked.
PREVIEW_CLI_DEPS := $(GUIDIR)/preview.c $(APPDIR)/logit.h $(VID_HDRS) \
                    c/lib/image/img.h c/apps/coreutils/logit_sniff.h \
                    $(VID_OBJ) $(MED_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) \
                    $(RUST_LIB) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm

$(BUILD)/previewplay.elf: $(PREVIEW_CLI_DEPS)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/previewplay.crt0c.o
	$(CC) $(UCFLAGS) -DPREVIEW_CLI -c $(GUIDIR)/preview.c -o $(BUILD)/apps/previewplay.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/previewplay.crt0c.o $(BUILD)/apps/previewplay.o \
	    $(VID_OBJ) $(MED_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(RUST_LIB) \
	    $(LIBC_OBJS) --end-group
$(BUILD)/previewplay.aex: $(BUILD)/previewplay.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/previewplay.elf $@ previewplay - 'P' 200 150 110

$(BUILD)/previewnegctl.elf: $(PREVIEW_CLI_DEPS)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/previewnegctl.crt0c.o
	$(CC) $(UCFLAGS) -DPREVIEW_CLI -DPREVIEW_NO_ANIM_TIMING -c $(GUIDIR)/preview.c \
	    -o $(BUILD)/apps/previewnegctl.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ --start-group \
	    $(BUILD)/apps/previewnegctl.crt0c.o $(BUILD)/apps/previewnegctl.o \
	    $(VID_OBJ) $(MED_OBJ) $(AUD_OBJ) $(IMGCHK_OBJ) $(RUST_LIB) \
	    $(LIBC_OBJS) --end-group
$(BUILD)/previewnegctl.aex: $(BUILD)/previewnegctl.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/previewnegctl.elf $@ previewnegctl - 'P' 200 150 110

# THE POSITIVE MEASUREMENT RUNS ON THE SHIPPED GUI BUILD, over QMP, because
# that is the player a user gets. The serial harness below cannot currently run
# it: a shell-spawned process that owns a window and then yields or sleeps never
# runs again on this kernel (see the note in tests/boot/run-preview-timing.sh --
# it is handed to the scheduler line, not worked around here). The NEGATIVE
# control is unaffected, because the build it exercises never waits at all,
# which is the whole point of it.
test-preview-timing: $(ISO) $(DISK) $(BUILD)/previewref/.stamp
	@python3 tests/qmp/qmp_preview.py --iso $(ISO) --disk $(DISK) \
	    --ref $(BUILD)/previewref --timing-only

test-preview-negctl: $(ISO) $(DISK)
	@bash tests/boot/run-preview-timing.sh $(ISO) $(DISK) negctl

# THE ASSOCIATION, from the other end. test-preview drives Preview's own list;
# this drives the route a user takes -- SYS_OPEN_PATH, which is all a Finder
# double-click is -- for one file of each family, and asserts the app was
# launched, was handed the path as its ARGUMENT, and decoded it.
test-preview-assoc: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_preview.py --iso $(ISO) --disk $(DISK) --assoc

.PHONY: test-preview test-preview-timing test-preview-negctl test-preview-assoc
