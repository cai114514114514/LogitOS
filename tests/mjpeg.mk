# tests/mjpeg.mk -- Motion JPEG (c/lib/video/mjpeg.c), which reuses
# c/lib/image/jpeg.c wholesale rather than decoding anything itself.
#
#   make test-mjpeg          the gate: byte-exact vs djpeg on every frame of
#                             every fixture, both with the frame's own DHT
#                             (ordinary path) and with it stripped (the
#                             default-Huffman-table splice, the AVI "MJPG"
#                             convention) -- plus reuse-buffer pointer
#                             identity across frames, mid-stream size-change
#                             refusal, and two-field-chunk detection.
#   make test-mjpeg-negctl   -DMJPEG_NO_DEFAULT_DHT: every DHT-stripped
#                             fixture must fail EVERY frame with the NAMED
#                             error MJPEG_ERR_NO_DHT, and every ordinary
#                             (DHT-present) fixture must stay exactly as
#                             green as it is under test-mjpeg.
#
# WHY A FRAGMENT AND NOT LINES IN THE MAKEFILE: this codec (mjpeg.c/.h, this
# file, tools/genmjpeg.sh) is being written beside a parallel workflow that
# owns c/lib/video/*, the other tests/*.mk fragments and the Makefile itself
# -- a whole-file Makefile write from a concurrent line has silently deleted
# other people's targets in this tree before (see CLAUDE.md). Add
#
#     -include tests/mjpeg.mk
#     ci-host: test-mjpeg
#
# ci-host: test-mjpeg AND NOT `ci-host: test-mjpeg test-mjpeg-negctl`, which is
# what this line said until 2026-08-28 and what shipped. audit_tests.py's NOT_CI
# drops every `test-*-negctl` from the list tools/ci.sh actually runs, on the
# stated assumption that a control is "RUN BY its positive counterpart" -- so
# naming the control there excludes it from CI and invokes it from nowhere, and
# it ran NEVER while looking wired. The control is a PREREQUISITE of test-mjpeg
# below instead, which is what makes that assumption true here rather than
# merely asserted (tests/license.mk and tests/logreporter.mk are the worked
# examples). Found by the audit's STRANDED CONTROLS category, which named this
# target the day it was written and five others beside it.
#
# (test-jpeg itself is NOT on any ci-host: line as of 2026-08-21 -- it is
# invoked directly, e.g. by the `test-img:` aggregate at Makefile:3527 -- so
# there is no existing line to piggyback on; `ci-host:` accepts prerequisites
# from any fragment and a target may declare it more than once, which is how
# every other fragment in tests/*.mk wires itself in without touching a line
# it does not own.) See the top-level integration report for the exact
# demux.c / media.h / Preview / js_media*.c lines this codec still needs
# (registration is deliberately NOT done here).
#
# This fragment reuses $(IMG_HOST_SRC), $(IMG_HOST_INC), $(RUST_LIB_HOST),
# $(CC) and $(BUILD) from the main Makefile (the same variables test-jpeg
# already builds against), so it must be loaded WITH it, not alone:
#
#     make -f Makefile -f tests/mjpeg.mk test-mjpeg
#     make -f Makefile -f tests/mjpeg.mk test-mjpeg-negctl
#
# Needs: everything test-jpeg needs (djpeg -- NOT PIL: every fixture here is
# real ffmpeg-encoded MJPEG, not a PIL-encoded still) plus ffmpeg itself, all
# on PATH. See tools/genmjpeg.sh's own header for what it builds and why.



MJPEG_DIR := $(BUILD)/mjpegtest
MJPEG_SRC := tests/unit/mjpeg_test.c c/lib/video/mjpeg.c $(IMG_HOST_SRC)
MJPEG_INC := -Ic/lib/video $(IMG_HOST_INC)


