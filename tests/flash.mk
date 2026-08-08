# --- test-flash: is a half-drawn window ever on screen? ---------------------
#
# A window's canvas is a SINGLE buffer: the app draws into the same memory the
# compositor blits from, and SYS_GUI_FLUSH swaps nothing. So an app that gets a
# burst of events -- a scroll, or the press and release of one click -- starts
# erasing the canvas for its next frame before the compositor has drawn the
# last one, and the compositor puts a blank window on the display.
#
# This is a PIXEL test, not a counter test: a frame counter cannot tell a
# correct repaint from a torn one, because both are one frame. The driver
# photographs the screen ~150 times a second through the gesture and asks how
# much colour is left in the window.
test-flash: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_flash.py

# THE NEGATIVE CONTROL, and it is meant to fail. Builds the same kernel with
# WM_MIDFRAME_GUARD flipped to 0 in a throwaway copy of the tree -- the
# behaviour this machine had before -- and requires the pixel check to catch a
# blank window. If it does not, the check above is a claim about a test that
# has never once failed.
test-flash-negctl: $(ISO) $(DISK)
	@python3 tests/qmp/qmp_flash.py --negative

.PHONY: test-flash test-flash-negctl
