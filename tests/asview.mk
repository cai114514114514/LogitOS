# asview -- UNIT IV's gate: an image viewer written in AetherScript, on the
# device, asserted against the pixels it drew.
#
# Own fragment for the reason tests/preview.mk and tests/h265.mk give: the root
# Makefile -- and in particular its $(DISK) recipe -- is a file several lines
# touch at once, and a target appended there is a merge conflict waiting to
# happen.
#
# NOTHING IS ADDED TO THE DISK HERE, and that is worth saying out loud because
# every other fragment in this directory has an `FS_FILES +=` line. This unit
# needs no new on-disk file at all:
#
#   * the program (fsroot/as/examples/asview.as) and the two library modules it
#     adds (fsroot/as/lib/image.as, and the extensions to lib/gui.as) are
#     picked up by the root Makefile's AS_EXAMPLES / AS_LIB_SRCS WILDCARDS, so
#     they are packed to /usr/as/examples/ and /usr/as/lib/ (source AND
#     precompiled .la) with no build-system change whatsoever;
#   * the picture it opens is $(BUILD)/dot.png, already packed at /media/dot.png
#     for the rich-terminal pixel test -- a solid 60x40 rectangle in a colour
#     nothing else in the UI uses, which is precisely what makes the assertion
#     in qmp_asview.py an exact pixel count instead of a tolerance;
#   * the non-image the negative control feeds it is /media/sample.h264, packed
#     for the video tests.
#
# Reusing all three is not thrift. A fixture generated for this gate would be a
# fixture only this gate has ever decoded, and "the viewer displays the file we
# made for the viewer" is a weaker sentence than "the viewer displays the file
# the terminal test measures independently".

ASVIEW_SRC := fsroot/as/examples/asview.as fsroot/as/lib/image.as \
              fsroot/as/lib/gui.as
ASVIEW_DEPS := $(ISO) $(DISK) tests/qmp/qmp_asview.py tests/qmp/qmp_ui.py \
               tests/boot/run-asview-test.sh $(ASVIEW_SRC)

# The whole gate: the picture, the key, and both refusals.
test-asview: $(ASVIEW_DEPS)
	@bash tests/boot/run-asview-test.sh $(ISO) $(DISK) all

# The halves, for bisecting a failure without paying for two boots.
test-asview-draw: $(ASVIEW_DEPS)
	@bash tests/boot/run-asview-test.sh $(ISO) $(DISK) fit

# The negative control, run as an ordinary passing case -- see the long note at
# the top of run-asview-test.sh for why inverting it would be backwards, and
# what it measures instead so that "drew nothing" cannot pass as "refused".
test-asview-negctl: $(ASVIEW_DEPS)
	@bash tests/boot/run-asview-test.sh $(ISO) $(DISK) refuse

.PHONY: test-asview test-asview-draw test-asview-negctl
