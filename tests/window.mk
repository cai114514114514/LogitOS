# --- window management: resize, zoom, minimise, system shortcuts -------------
#
# Its own fragment rather than lines in the Makefile, for the reason every other
# fragment here says: written straight into that file, a whole-file overwrite
# from a concurrent line deletes it within the hour.
#
# test-window drives a real PS/2 mouse and a real PS/2 keyboard over QMP: eight
# edge and corner drags (each one out and straight back, which is the check for
# the anchor-not-accumulate rule inside the drag), the minimum size, a
# grow-and-shrink round trip photographed at both ends, zoom via the green light
# and via a titlebar double-click, restore-to-the-exact-frame, minimise, and the
# shortcut table -- including the pair that demonstrates the CLAIM RULE, which
# is that Cmd+K reaches the focused app and Cmd+W does not.
#
# Geometry is asserted against the guest's own `[wm] win ...` report rather than
# reconstructed from a screendump. Pixels are asserted for the one thing pixels
# are the authority on: that a window which shrank left nothing behind.
test-window: $(ISO) $(DISK)
	python3 tests/qmp/qmp_window.py --iso $(ISO)

# THE NEGATIVE CONTROL, and it is meant to fail. It rebuilds the kernel with
# WM_RESIZE_DAMAGE_LIE=1 in a throwaway copy of the tree -- a compositor that
# reports the NEW window box honestly and forgets the OLD one, which is the
# specific mistake a resize invites: when a window shrinks the two boxes are
# nested, so every counter-based check still passes while a band of the previous
# frame sits on the wallpaper. The round-trip pixel check must then fail. The
# target succeeds when the test fails.
test-window-negctl: $(ISO) $(DISK)
	python3 tests/qmp/qmp_window.py --iso $(ISO) --negative

# What a resize COSTS, measured on the machine at three resolutions rather than
# estimated: the driver brackets a resize drag with the compositor's own
# counters and prints ms and pixels per composite. The question it exists to
# answer is whether live resize is usable under TCG, and it is the sort of
# question that is answered wrong by assuming either way.
bench-window: $(ISO) $(DISK)
	python3 tests/qmp/qmp_window.py --iso $(ISO) --bench --xres 1280 --yres 800
	python3 tests/qmp/qmp_window.py --iso $(ISO) --bench --xres 1920 --yres 1200
	python3 tests/qmp/qmp_window.py --iso $(ISO) --bench --xres 2560 --yres 1600

# Does every GUI app in the tree survive being resized? Opens each one, grows
# it, and reports what the newly exposed region contains. Not a pass/fail gate
# on apps this change does not own -- it is the inventory, with screenshots.
test-window-apps: $(ISO) $(DISK)
	python3 tests/qmp/qmp_window.py --iso $(ISO) --apps --shots $(BUILD)/window-shots

.PHONY: test-window test-window-negctl bench-window test-window-apps
