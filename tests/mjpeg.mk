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
#     ci-host: test-mjpeg test-mjpeg-negctl
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

.PHONY: test-mjpeg test-mjpeg-negctl

MJPEG_DIR := $(BUILD)/mjpegtest
MJPEG_SRC := tests/unit/mjpeg_test.c c/lib/video/mjpeg.c $(IMG_HOST_SRC)
MJPEG_INC := -Ic/lib/video $(IMG_HOST_INC)

test-mjpeg: $(RUST_LIB_HOST)
	@mkdir -p $(MJPEG_DIR)
	@bash tools/genmjpeg.sh $(MJPEG_DIR)/fixtures
	@$(CC) -O2 -w -o $(MJPEG_DIR)/mjpeg_test $(MJPEG_SRC) $(RUST_LIB_HOST) $(MJPEG_INC)
	@$(MJPEG_DIR)/mjpeg_test $(MJPEG_DIR)/fixtures

# The one negative control the task specifies. jpeg.c already refuses a frame
# with an undefined Huffman table (correctly -- a STILL JPEG missing DHT really
# is malformed) but that refusal is indistinguishable from ordinary corruption:
# it says nothing about WHY. -DMJPEG_NO_DEFAULT_DHT compiles the default-table
# splice out of mjpeg_decode_frame, so the reason becomes visible as a named
# error instead. Every frame of every *_nodht.mjpeg fixture must fail with
# MJPEG_ERR_NO_DHT specifically -- 17 of them, one per frame across the 5 cases
# tools/genmjpeg.sh builds (4+4+4+3+2), because EVERY frame of a genuinely
# tables-stripped stream lacks DHT, not only the first one a real caller would
# reach. Every plain *.mjpeg fixture must decode exactly as green as it does
# under test-mjpeg -- proving the switch touches only the path it names, the
# same shape test-jpeg-negctl already requires of its own two controls.
test-mjpeg-negctl: $(RUST_LIB_HOST)
	@mkdir -p $(MJPEG_DIR)
	@bash tools/genmjpeg.sh $(MJPEG_DIR)/fixtures >/dev/null
	@$(CC) -O2 -w -DMJPEG_NO_DEFAULT_DHT -o $(MJPEG_DIR)/mjpeg_negctl $(MJPEG_SRC) \
	    $(RUST_LIB_HOST) $(MJPEG_INC)
	@if $(MJPEG_DIR)/mjpeg_negctl $(MJPEG_DIR)/fixtures >$(MJPEG_DIR)/negctl.log 2>&1; then \
	   echo "FAIL: -DMJPEG_NO_DEFAULT_DHT still passes -- the control proves nothing"; exit 1; \
	 fi
	@n=`grep -c '^FAIL' $(MJPEG_DIR)/negctl.log`; \
	 named=`grep '^FAIL' $(MJPEG_DIR)/negctl.log | grep -c 'MJPEG_NO_DEFAULT_DHT'`; \
	 echo "-DMJPEG_NO_DEFAULT_DHT: $$n FAIL line(s), $$named named MJPEG_ERR_NO_DHT"; \
	 if [ "$$named" != "$$n" ]; then \
	   echo "FAIL: a failure was NOT named MJPEG_ERR_NO_DHT -- not testing the DHT splice"; \
	   grep '^FAIL' $(MJPEG_DIR)/negctl.log | grep -v 'MJPEG_NO_DEFAULT_DHT'; exit 1; \
	 fi; \
	 if [ "$$n" != "17" ]; then \
	   echo "FAIL: expected exactly 17 FAILs (one per frame of every *_nodht case), got $$n"; \
	   exit 1; \
	 fi; \
	 echo "ok: exactly 17 nodht frames fail, all named MJPEG_ERR_NO_DHT, nothing else touched"
