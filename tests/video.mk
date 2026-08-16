# tests/video.mk -- the decode-THROUGHPUT benchmark: frames/sec by resolution,
# for H.264 and H.265, measured on the host AND on LogitOS itself under QEMU
# TCG. Kept out of the main Makefile for the same reason tests/h264.mk and
# tests/h265.mk are: several people touch this tree at once, and a 2500-line
# shared file is where their edits collide. The root Makefile carries one
# `-include tests/video.mk` line (next to the h264.mk/h265.mk ones) and
# nothing else.
#
# THIS FILE IS NOT A CORRECTNESS GATE. test-h264/test-h265 already own bit-
# exactness against ffmpeg; nothing here re-checks a pixel. c/apps/video/
# vidbench.c decodes real streams and times the wall clock, nothing more --
# and it says so loudly (VIDBENCH-ERR) rather than silently skipping a stream
# it cannot decode, because a benchmark that quietly drops the hard case
# produces a table that looks complete and is not.
#
# WHY TWO MACHINES: TCG is the machine this OS actually runs on, so the guest
# number is the one every plan for video playback has to respect. The host
# number is easy to get and nearly useless on its own -- it exists so the
# HOST/GUEST ratio is known, which is what lets a future host-only measurement
# (no QEMU boot required) predict a guest number.

.PHONY: test-vidbench test-vidbench-host test-vidbench-guest

VIDBENCH_INC   := -Ic/lib/video
VIDBENCH_SRC   := $(wildcard c/lib/video/*.c)
VIDBENCH_DIR   := $(BUILD)/vidbenchref
VIDBENCH_SIZES := 320x240 640x360 1280x720 1920x1080
VIDBENCH_H264  := $(foreach s,$(VIDBENCH_SIZES),$(VIDBENCH_DIR)/h264-$(s).h264)
VIDBENCH_H265  := $(foreach s,$(VIDBENCH_SIZES),$(VIDBENCH_DIR)/h265-$(s).h265)
VIDBENCH_STREAMS := $(VIDBENCH_H264) $(VIDBENCH_H265)

# One rule producing all 8 streams (genvidbench.sh is idempotent and re-checks
# each file itself); a stamp file is what lets `make` treat that as a single
# recipe run instead of one per output, which a naive multi-target pattern
# would race under `-j`.
$(VIDBENCH_DIR)/.stamp: tools/genvidbench.sh
	@mkdir -p $(VIDBENCH_DIR)
	@bash tools/genvidbench.sh $(VIDBENCH_DIR)
	@touch $@
$(VIDBENCH_STREAMS): $(VIDBENCH_DIR)/.stamp

# --- host build --------------------------------------------------------------
# Plain $(CC): no --target/-ffreestanding, so this is a NATIVE host binary
# (same trick tests/h264.mk's h264_test and tests/h265.mk's h265_test already
# use) linking the identical decoder sources the guest build links.
$(BUILD)/vidbench_host: c/apps/video/vidbench.c $(VIDBENCH_SRC)
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ c/apps/video/vidbench.c $(VIDBENCH_SRC) $(VIDBENCH_INC)

test-vidbench-host: $(BUILD)/vidbench_host $(VIDBENCH_STREAMS)
	@echo "--- vidbench: host, run 1 ---"
	@$(BUILD)/vidbench_host $(VIDBENCH_STREAMS)
	@echo "--- vidbench: host, run 2 (reproducibility) ---"
	@$(BUILD)/vidbench_host $(VIDBENCH_STREAMS)

# --- guest build ---------------------------------------------------------
# Same shape as vidcheck.aex/vidcheck265.aex (root Makefile / tests/h265.mk):
# $(BUILD)/vidobj/c/apps/video/vidbench.o comes from the Makefile's existing
# generic `$(BUILD)/vidobj/%.o: %.c $(VID_HDRS)` pattern rule, no new rule
# needed for it. Links VID_OBJ (both decoders -- c/lib/video/*.c is a
# wildcard) + the default-arena LIBC_OBJS, exactly like vidcheck.aex; nothing
# in this benchmark's stream matrix needed a bigger arena (see genvidbench.sh
# for the H.265 feature set it deliberately stays inside), so it does not ask
# for one -- a special arena here would be measuring a machine no real app on
# this OS is built with.
$(BUILD)/vidbench.elf: $(BUILD)/vidobj/c/apps/video/vidbench.o $(VID_OBJ) $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/vidbench.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ $(BUILD)/apps/vidbench.crt0c.o \
	    $(BUILD)/vidobj/c/apps/video/vidbench.o $(VID_OBJ) $(LIBC_OBJS)
$(BUILD)/vidbench.aex: $(BUILD)/vidbench.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/vidbench.elf $@ vidbench - 'V' 150 150 150

# Onto the disk image without editing the root Makefile's $(DISK) recipe:
# FS_FILES is expanded when that recipe runs (after every -include is read),
# so appending here is enough, and the prerequisites go on a second,
# recipe-less rule for the same target -- the exact mechanism tests/h265.mk
# already documents and uses for vidcheck265.
FS_FILES += $(BUILD)/vidbench.aex:/bin/vidbench \
            $(VIDBENCH_DIR)/h264-320x240.h264:/media/vidbench/h264-320x240.h264 \
            $(VIDBENCH_DIR)/h264-640x360.h264:/media/vidbench/h264-640x360.h264 \
            $(VIDBENCH_DIR)/h264-1280x720.h264:/media/vidbench/h264-1280x720.h264 \
            $(VIDBENCH_DIR)/h264-1920x1080.h264:/media/vidbench/h264-1920x1080.h264 \
            $(VIDBENCH_DIR)/h265-320x240.h265:/media/vidbench/h265-320x240.h265 \
            $(VIDBENCH_DIR)/h265-640x360.h265:/media/vidbench/h265-640x360.h265 \
            $(VIDBENCH_DIR)/h265-1280x720.h265:/media/vidbench/h265-1280x720.h265 \
            $(VIDBENCH_DIR)/h265-1920x1080.h265:/media/vidbench/h265-1920x1080.h265
$(DISK): $(BUILD)/vidbench.aex $(VIDBENCH_STREAMS)

test-vidbench-guest: $(ISO) $(DISK)
	@bash tests/boot/run-vidbench-test.sh $(ISO) $(DISK)

# --- the target the task GATE runs: both machines, one table -----------------
test-vidbench: test-vidbench-host test-vidbench-guest
	@echo "vidbench: host + guest tables printed above"
