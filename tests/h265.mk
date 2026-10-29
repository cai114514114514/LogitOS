# tests/h265.mk -- the H.265/HEVC decoder's gates.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 1800-line file is where their edits collide. The
# root Makefile carries one `-include tests/h265.mk` line and nothing else,
# which is the same arrangement tests/nic.mk, tests/audio.mk and tests/usb.mk
# already use.

.PHONY: test-h265-units-control \
        test-h265-diff test-h265-b test-video265 test-h265-m10 \
        test-h265-bounds test-h265-bounds-negctl

H265_SRC := c/lib/video/h265.c c/lib/video/h265_nal.c c/lib/video/h265_cabac.c \
            c/lib/video/h265_pred.c c/lib/video/h265_mc.c c/lib/video/h265_deblock.c
H265_INC := -Ic/lib/video

# The cases `make test-h265` requires to be BIT-EXACT. Anything the decoder does
# not yet get exactly right is not in this list and is not claimed anywhere; run
# `make test-h265-diff` to see the whole matrix including the failures, which is
# the honest picture.
H265_GATE := i-only-160x120 i-only-322x242 i-nosao-160x120 i-raw-160x120 \
             i-ctb16-160x120 ip-320x240 ip-640x360 ip-refs4 ip-notmvp ip-amp \
             ip-longgop ip-tskip ip-wpp

# The Main 10 half of the same list. Not a separate quality bar -- these are
# gated by `make test-h265` exactly like the 8-bit cases, because 10 bits is
# now a claimed feature rather than an experiment. Every one of them is
# compared as 16-BIT SAMPLES against ffmpeg's yuv420p10le output.
#
# m10-bright / m10-bright-ip are the clamp cases: content pushed against the
# top of the 10-bit range, where a Clip1 left at 255 stops being invisible.
# m10-scaling is NOT here -- scaling lists are wrong at BOTH depths, see the
# note on test-h265-scaling below.
H265_GATE10 := m10-i-160x120 m10-i-qp12 m10-i-qp40 m10-i-322x242 \
               m10-bright m10-bright-ip \
               m10-ip-320x240 m10-ip-640x360 m10-ip-refs4 m10-ip-amp \
               m10-tskip m10-wpp


$(BUILD)/h265_test: tests/unit/h265test.c $(H265_SRC) c/lib/video/h265.h \
                    c/lib/video/h265_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/h265test.c $(H265_SRC) $(H265_INC)

# Both audit findings were already guarded, but neither had a regression
# gate. The controls below compile each historical omission into the same
# decoder: the RPS case must trip UBSan at keep[34], and the malformed SPS
# must be accepted. A merely nonzero exit is insufficient (e.g. an ASan host
# startup failure); each control must print its named failure.
H265_BOUNDS_SRC := tests/unit/h265_bounds_test.c \
                   $(filter-out c/lib/video/h265.c c/lib/video/h265_nal.c,$(H265_SRC))
H265_BOUNDS_FLAGS := -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
                     -Wall -Wextra $(H265_INC)

$(BUILD)/h265_bounds_test: $(H265_BOUNDS_SRC) c/lib/video/h265.c c/lib/video/h265_nal.c \
                           c/lib/video/h265_int.h c/lib/video/bs.h
	@mkdir -p $(BUILD)
	@$(CC) $(H265_BOUNDS_FLAGS) -o $@ $(H265_BOUNDS_SRC)



# --- the bisection tool -----------------------------------------------------
# Total wrong bytes per case, per plane, plus the first three bad pictures and
# the largest single-sample error. THIS is the number to compare variants
# against: on a decoder with two in-loop filters a real fix routinely moves the
# first mismatch EARLIER while removing 90% of the wrong bytes, so "the first
# mismatch moved" is not evidence of anything. Never gates; always reports.
test-h265-diff: $(BUILD)/h265_diff
	@mkdir -p $(BUILD)/h265ref
	@bash tools/genvideo265.sh $(BUILD)/h265ref all || exit 0
	@for f in $(BUILD)/h265ref/*.h265; do \
	    printf '%-20s ' "$$(basename $$f .h265)"; \
	    $(BUILD)/h265_diff $$f $${f%.h265}.ref.yuv | tail -1; \
	 done

$(BUILD)/h265_diff: tests/unit/h265_diff.c $(H265_SRC) c/lib/video/h265.h \
                    c/lib/video/h265_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/h265_diff.c $(H265_SRC) $(H265_INC)


# --- the decoder on the device ----------------------------------------------
# /bin/vidcheck265 links the SAME decoder sources against mini-libc for ring 3
# (they are already in VID_OBJ -- c/lib/video/*.c is a wildcard) and prints the
# same CRC32 the host driver prints. Everything above this line is a glibc
# build on Linux; only this says the decoder works on LogitOS.
#
# It gets onto the disk image WITHOUT editing the root Makefile's $(DISK)
# recipe: FS_FILES is expanded when that recipe runs, which is after every
# -include has been read, and mkfs.py takes host[:/dest] for any argument. So
# appending here is enough, and the prerequisites go on a second, recipe-less
# rule for the same target. This tree is worked on by several people at once
# and $(DISK) is a line they all touch; not touching it is the point.
FS_FILES += $(BUILD)/vidcheck265.aex:/bin/vidcheck265 \
            tests/fixtures/video265/sample.h265:/media/sample.h265
$(DISK): $(BUILD)/vidcheck265.aex tests/fixtures/video265/sample.h265
$(BUILD)/vidcheck265.elf: $(BUILD)/vidobj/tests/guest/video/vidcheck265.o $(VID_OBJ) $(IMGCHK_OBJ) $(GFX_OBJ) $(RUST_LIB) \
                          $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/vidcheck265.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/vidcheck265.crt0c.o \
	    $(BUILD)/vidobj/tests/guest/video/vidcheck265.o $(VID_OBJ) $(IMGCHK_OBJ) \
	    $(GFX_OBJ) $(RUST_LIB) $(LIBC_OBJS)
$(BUILD)/vidcheck265.aex: $(BUILD)/vidcheck265.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/vidcheck265.elf $@ vidcheck265 - 'V' 150 150 150






# The negative control. The SAME test file against h265_mc.c built with
# -DH265_CONTROL_NO_14BIT, which rounds the horizontal interpolation pass back
# to 8-bit samples before the vertical one -- the textbook HEVC MC bug, wrong
# on every half-pel diagonal and visually almost invisible. This target PASSES
# when the test FAILS. If it ever stops failing, test-h265-units is not
# measuring the intermediate precision it claims to.
test-h265-units-control:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -DH265_CONTROL_NO_14BIT -o $(BUILD)/h265_mc_control \
	    tests/unit/h265_mc_test.c c/lib/video/h265_mc.c $(H265_INC)
	@if $(BUILD)/h265_mc_control > $(BUILD)/h265_mc_control.log 2>&1; then \
	    echo "CONTROL-FAIL: h265_mc_test passed with the 14-bit intermediate removed"; \
	    exit 1; \
	 else \
	    echo "CONTROL-OK: h265_mc_test fails without the 14-bit intermediate"; \
	    head -3 $(BUILD)/h265_mc_control.log; \
	 fi

