# tests/h265.mk -- the H.265/HEVC decoder's gates.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 1800-line file is where their edits collide. The
# root Makefile carries one `-include tests/h265.mk` line and nothing else,
# which is the same arrangement tests/nic.mk, tests/audio.mk and tests/usb.mk
# already use.

.PHONY: test-h265 test-h265-units test-h265-units-control test-h265-units-asan \
        test-h265-diff test-h265-b test-video265

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

# --- whole-stream gate ------------------------------------------------------
# Bit-exact against ffmpeg's own HEVC decoder, every byte of every picture.
# HEVC reconstruction is exactly specified integer arithmetic, so a tolerance
# would be a decision to stop measuring.
#
# The generated matrix needs ffmpeg WITH libx265. When that is missing,
# genvideo265.sh exits 3 and the generated half is skipped -- but the committed
# fixture below still runs, so this target means something on a machine with no
# encoder installed. That is the same reason tests/fixtures/video/sample.h264
# exists for H.264.
test-h265: $(BUILD)/h265_test
	@mkdir -p $(BUILD)/h265ref
	@rc=0; bash tools/genvideo265.sh $(BUILD)/h265ref core || rc=$$?; \
	 if [ $$rc = 3 ]; then \
	    echo "H265: no libx265 -- generated matrix skipped, fixture only"; \
	 elif [ $$rc != 0 ]; then exit $$rc; \
	 else \
	    for c in $(H265_GATE); do \
	        $(BUILD)/h265_test $(BUILD)/h265ref/$$c.h265 \
	            $(BUILD)/h265ref/$$c.ref.yuv || exit 1; \
	    done; \
	 fi
	@# The committed fixture: a stream and the CRC32 of its correct decode, so
	@# this gate still measures something with no encoder on the machine. The
	@# CRC was pinned from a decode verified bit-exact against ffmpeg.
	@crc=`$(BUILD)/h265_test tests/fixtures/video265/sample.h265 | awk '{print $$2}'`; \
	 want=`cat tests/fixtures/video265/sample.crc32`; \
	 if [ "$$crc" != "$$want" ]; then \
	    echo "H265-FAIL fixture crc $$crc want $$want"; exit 1; fi; \
	 echo "H265-OK fixture crc $$crc (tests/fixtures/video265/sample.h265)"

$(BUILD)/h265_test: tests/unit/h265test.c $(H265_SRC) c/lib/video/h265.h \
                    c/lib/video/h265_int.h
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $@ tests/unit/h265test.c $(H265_SRC) $(H265_INC)

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

# --- the WHOLE decoder under ASan/UBSan --------------------------------------
# The per-module ASan target above covers three leaf modules. This runs the
# orchestrator -- the CTU quadtree, the DPB, the tile and z-scan tables, all the
# malloc/free of reference pictures -- over every stream in the matrix,
# INCLUDING the ones it decodes wrongly and the one it refuses as corrupt, since
# error paths are where a decoder leaks or reads past a buffer. Every input byte
# is untrusted; this is the target that says so with evidence.
test-h265-asan:
	@mkdir -p $(BUILD)/h265ref
	@bash tools/genvideo265.sh $(BUILD)/h265ref all || exit 0
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -w \
	    -o $(BUILD)/h265_test_asan tests/unit/h265test.c $(H265_SRC) $(H265_INC)
	@rc=0; for f in $(BUILD)/h265ref/*.h265 tests/fixtures/video265/sample.h265; do \
	    out=`ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=print_stacktrace=1 \
	         $(BUILD)/h265_test_asan $$f 2>&1`; \
	    if echo "$$out" | grep -q 'runtime error\|AddressSanitizer\|LeakSanitizer'; then \
	        echo "SANITIZER: `basename $$f`"; echo "$$out" | head -12; rc=1; \
	    fi; \
	 done; \
	 [ $$rc = 0 ] && echo "test-h265-asan: clean over the whole matrix" || exit 1

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
$(BUILD)/vidcheck265.elf: $(BUILD)/vidobj/c/apps/video/vidcheck265.o $(VID_OBJ) \
                          $(LIBC_OBJS) $(APPDIR)/crt0_cli.asm
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0_cli.asm -o $(BUILD)/apps/vidcheck265.crt0c.o
	$(LD) -nostdlib -e _start -Ttext=0x50000000 -o $@ \
	    $(BUILD)/apps/vidcheck265.crt0c.o \
	    $(BUILD)/vidobj/c/apps/video/vidcheck265.o $(VID_OBJ) $(LIBC_OBJS)
$(BUILD)/vidcheck265.aex: $(BUILD)/vidcheck265.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/vidcheck265.elf $@ vidcheck265 - 'V' 150 150 150

test-video265: $(ISO) $(DISK)
	@bash tests/boot/run-video265-test.sh $(ISO) $(DISK)

# --- B slices, gated separately ---------------------------------------------
# Kept out of test-h265 until they are bit-exact, so that target never has to
# be read as "passes except for the part it silently skips".
test-h265-b: $(BUILD)/h265_test
	@mkdir -p $(BUILD)/h265ref
	@bash tools/genvideo265.sh $(BUILD)/h265ref b
	@for c in b-320x240 b-pyramid b-weighted; do \
	    $(BUILD)/h265_test $(BUILD)/h265ref/$$c.h265 \
	        $(BUILD)/h265ref/$$c.ref.yuv || exit 1; \
	 done

# --- per-module gates -------------------------------------------------------
# A whole-stream test says something is wrong; a module test says what. Each of
# these runs its module against something OTHER than a copy of itself: the
# CABAC engine against the spec's arithmetic *encoder*, the transform matrix
# against independently typed 4- and 8-point matrices, the interpolation
# filters against their own impulse response and against a reference written
# from the spec's clamped formulation.
test-h265-units:
	@mkdir -p $(BUILD)
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h265_cabac_test \
	    tests/unit/h265_cabac_test.c c/lib/video/h265_cabac.c $(H265_INC)
	@$(BUILD)/h265_cabac_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h265_pred_test \
	    tests/unit/h265_pred_test.c c/lib/video/h265_pred.c $(H265_INC)
	@$(BUILD)/h265_pred_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h265_mc_test \
	    tests/unit/h265_mc_test.c c/lib/video/h265_mc.c $(H265_INC)
	@$(BUILD)/h265_mc_test
	@$(CC) -O2 -Wall -Wextra -o $(BUILD)/h265_deblock_test \
	    tests/unit/h265_deblock_test.c c/lib/video/h265_deblock.c $(H265_INC)
	@$(BUILD)/h265_deblock_test

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

# Same three, under ASan/UBSan. The interpolation module indexes a padded
# plane by hand and falls back to an emulated copy for far-out vectors, which
# is precisely the shape of code that reads one row too far without ever
# producing a wrong pixel on the host.
test-h265-units-asan:
	@mkdir -p $(BUILD)
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -o $(BUILD)/h265_mc_asan \
	    tests/unit/h265_mc_test.c c/lib/video/h265_mc.c $(H265_INC)
	@UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/h265_mc_asan
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -o $(BUILD)/h265_pred_asan \
	    tests/unit/h265_pred_test.c c/lib/video/h265_pred.c $(H265_INC)
	@UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/h265_pred_asan
	@$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
	    -Wall -Wextra -o $(BUILD)/h265_cabac_asan \
	    tests/unit/h265_cabac_test.c c/lib/video/h265_cabac.c $(H265_INC)
	@UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/h265_cabac_asan
