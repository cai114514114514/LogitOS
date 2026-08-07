# tests/h265.mk -- the H.265/HEVC decoder's gates.
#
# Kept out of the main Makefile on purpose: this tree is worked on by several
# people at once and a shared 1800-line file is where their edits collide. The
# root Makefile carries one `-include tests/h265.mk` line and nothing else,
# which is the same arrangement tests/nic.mk, tests/audio.mk and tests/usb.mk
# already use.

.PHONY: test-h265 test-h265-units test-h265-diff test-video265

H265_SRC := c/lib/video/h265.c c/lib/video/h265_nal.c c/lib/video/h265_cabac.c \
            c/lib/video/h265_pred.c c/lib/video/h265_mc.c c/lib/video/h265_deblock.c
H265_INC := -Ic/lib/video

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
