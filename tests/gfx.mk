# ============================================================================
# Open Logit -- the 2D rendering engine (c/lib/gfx). Tests and cost.
#
# THE BAR IS A NUMBER, NOT A PICTURE. Every filled shape is compared against a
# 16x16 supersampled evaluation of its own analytic predicate, and every blend
# against the Porter-Duff formula recomputed in double. Both references are
# written independently of the engine and share no line with it. That is the
# right answer for this domain: unlike H.264, where ffmpeg is the oracle, 2D
# coverage has no third party to diff against -- but it is exactly computable,
# so the oracle can simply be built.
#
#   make test-gfx          all three host suites (seconds, no QEMU)
#   make test-gfx-negctl   the same suite with antialiasing compiled out; it
#                          MUST fail, and the target succeeds when it does
#   make test-gfx-stroke-negctl  the stroke suite with the JOIN machinery
#                          compiled out; same contract, different mechanism
#   make bench-gfx         what the engine costs, per primitive, on the host
#   make bench-gfx-frame   what ONE APP FRAME costs on the machine, at three
#                          resolutions, with gui_clear and text separated out
# ============================================================================
.PHONY: test-gfx test-gfx-clip test-gfx-clip-negctl test-gfx-raster test-gfx-paint test-gfx-stroke test-gfx-negctl \
        test-gfx-stroke-negctl bench-gfx bench-gfx-stroke bench-gfx-frame

GFX_TEST_CF := -O1 -g -Wall -Wextra -Ic/lib/gfx

$(BUILD)/gfx_raster_test: tests/unit/gfx_raster_test.c $(GFX_SRC) c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -o $@ tests/unit/gfx_raster_test.c $(GFX_SRC) -lm

$(BUILD)/gfx_paint_test: tests/unit/gfx_paint_test.c $(GFX_SRC) c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -o $@ tests/unit/gfx_paint_test.c $(GFX_SRC) -lm

test-gfx-raster: $(BUILD)/gfx_raster_test
	$(BUILD)/gfx_raster_test

test-gfx-paint: $(BUILD)/gfx_paint_test
	$(BUILD)/gfx_paint_test

# The stroker. Its reference is a THIRD independent oracle: the distance from
# a pixel to the FLATTENED source polyline, plus the miter wedge and the
# square-cap half-square in closed form -- see the comment at the top of
# tests/unit/gfx_stroke_test.c for why the distance is to the polyline and
# never to the ideal curve (measuring against the curve charges the path's
# flattening error to the stroker and fails a correct one).
$(BUILD)/gfx_stroke_test: tests/unit/gfx_stroke_test.c $(GFX_SRC) c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -o $@ tests/unit/gfx_stroke_test.c $(GFX_SRC) -lm

test-gfx-stroke: $(BUILD)/gfx_stroke_test
	$(BUILD)/gfx_stroke_test

# Path clipping: coverage times coverage at the row_fn seam. Its reference is
# the AND of two existing analytic predicates, supersampled the same 16x16 way
# -- composition costs the oracle almost nothing, which is exactly why the
# in_sq_annulus pattern in gfx_raster_test.c was worth copying. The clip mask
# is materialized by the CALLER (raster() is not reentrant); the tests use
# deliberately ASYMMETRIC clip extents and origins, because a centred clip
# hides a transposed-axis or sign error and an off-by-one in either axis is
# the single most likely defect in this feature.
$(BUILD)/gfx_clip_test: tests/unit/gfx_clip_test.c $(GFX_SRC) c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -o $@ tests/unit/gfx_clip_test.c $(GFX_SRC) -lm

test-gfx-clip: $(BUILD)/gfx_clip_test
	$(BUILD)/gfx_clip_test

test-gfx: test-gfx-raster test-gfx-paint test-gfx-stroke test-gfx-clip

# The NEGATIVE CONTROL, and it is meant to fail. -DGFX_NO_AA drops the
# rasterizer to a single centre sample per row with binary horizontal coverage
# -- every shape still draws, and still looks broadly right, which is precisely
# the point: what breaks is the AGREEMENT WITH THE REFERENCE. If the accuracy
# assertions still passed without antialiasing they would be measuring the
# geometry, not the rasterizer. The target succeeds when the test fails.
test-gfx-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -DGFX_NO_AA -o $(BUILD)/gfx_raster_negctl \
	    tests/unit/gfx_raster_test.c $(GFX_SRC) -lm
	@echo "--- negative control: the SAME assertions with antialiasing compiled out ---"
	@if $(BUILD)/gfx_raster_negctl > $(BUILD)/gfx_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the hard-edged build matched the reference"; \
	    exit 1; \
	else \
	    grep -c '^FAIL' $(BUILD)/gfx_negctl.log | \
	        xargs -I{} echo "negative control ok: {} assertion(s) fail without antialiasing"; \
	fi

# The STROKER's negative control, and it is a different mechanism from
# -DGFX_NO_AA on purpose. Antialiasing is not what a join is; compiling the
# rasterizer down would fail the stroke assertions for a reason that has
# nothing to do with joins, and would prove nothing about them.
# -DGFX_NO_JOINS instead strokes EVERY SEGMENT ON ITS OWN, butt-connected: no
# wedge filled at a corner, no miter tip formed, no arc swept. Caps, dashes
# and the zero-length dot are deliberately left working, so the failures name
# the join machinery and only the join machinery. Every stroke still draws and
# still looks broadly like a stroke -- what breaks is the agreement with the
# reference at every corner. The target succeeds when the test fails.
test-gfx-stroke-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -DGFX_NO_JOINS -o $(BUILD)/gfx_stroke_negctl \
	    tests/unit/gfx_stroke_test.c $(GFX_SRC) -lm
	@echo "--- negative control: the SAME assertions with the joins compiled out ---"
	@if $(BUILD)/gfx_stroke_negctl > $(BUILD)/gfx_stroke_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: butt-connected segments matched the reference"; \
	    exit 1; \
	else \
	    grep -c '^FAIL' $(BUILD)/gfx_stroke_negctl.log | \
	        xargs -I{} echo "negative control ok: {} assertion(s) fail without joins"; \
	fi

# -DGFX_NO_CLIP bypasses the row multiply: the clipped entry points fill as if
# no mask were given. Every containment assertion must then fail. KNOWN,
# DISCLOSED PARTIALITY (from the adversarial pass): the define does not bypass
# the row-level out-of-extent early-out or the empty-intersection early
# return, so two of the disjoint-clip assertions pass under the control via
# mechanisms it never gates -- the control proves the MULTIPLY is load-
# bearing, not every containment mechanism at once. The target succeeds when
# the test fails.
test-gfx-clip-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GFX_TEST_CF) -DGFX_NO_CLIP -o $(BUILD)/gfx_clip_negctl \
	    tests/unit/gfx_clip_test.c $(GFX_SRC) -lm
	@echo "--- negative control: the SAME assertions with the clip multiply bypassed ---"
	@if $(BUILD)/gfx_clip_negctl > $(BUILD)/gfx_clip_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: an unclipped fill matched the clipped reference"; \
	    exit 1; \
	else \
	    grep -c '^FAIL' $(BUILD)/gfx_clip_negctl.log | \
	        xargs -I{} echo "negative control ok: {} assertion(s) fail without the clip"; \
	fi

# Per-primitive cost on the host. Not a substitute for bench-gfx-frame -- a host
# core is not the guest under TCG -- but it is where the engine's OWN share of a
# frame is separable from syscalls, and it is the number that moves when the
# rasterizer changes.
bench-gfx: $(BUILD)/gfx_bench
	$(BUILD)/gfx_bench

# What STROKING costs, with outline construction split from the fill -- the
# split matters because gfx_stroke_path is a path-to-path transform, so a
# caller redrawing fixed geometry can cache the outline and pay only the
# second number. It rides on the stroke test binary (-DGFX_STROKE_BENCH)
# rather than a second source file, so the geometry benchmarked is the exact
# geometry the accuracy assertions are written against.
bench-gfx-stroke:
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -Ic/lib/gfx -DGFX_STROKE_BENCH \
	    -o $(BUILD)/gfx_stroke_bench tests/unit/gfx_stroke_test.c $(GFX_SRC) -lm
	@$(BUILD)/gfx_stroke_bench | sed -n '/what a stroke costs/,$$p'

$(BUILD)/gfx_bench: tests/unit/gfx_bench.c $(GFX_SRC) c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)
	$(CC) -O2 -g -Wall -Wextra -Ic/lib/gfx -o $@ tests/unit/gfx_bench.c $(GFX_SRC) -lm

# What ONE FRAME costs on the machine, at 1280x800, 1920x1200 and 2560x1600,
# with the components separated -- because the toolkit line already measured
# 24-27 ms for a full-window repaint and found the dominant cost was
# aui_begin()'s unconditional gui_clear plus text, NOT the rasterized
# primitives. An honest comparison has to split those out or it credits the
# engine with a cost it does not pay and hides one it does.
#
# The split is measured, not modelled: -DAUI_COST builds aui.c with a
# CLOCK_MONOTONIC bracket around each class of call and prints the buckets on
# the serial console. That instrumentation costs a syscall per draw call and so
# inflates the total; the harness reports the uninstrumented total from
# bench-aui alongside it, and the buckets are to be read as a RATIO.
bench-gfx-frame: $(ISO) $(BUILD)/gallery_cost.aex
	$(MAKE) DISK=$(BUILD)/disk_gfxcost.img GALLERY_AEX=$(BUILD)/gallery_cost.aex \
	    $(BUILD)/disk_gfxcost.img
	bash tests/boot/run-gfx-bench.sh $(ISO) $(BUILD)/disk_gfxcost.img

$(BUILD)/apps/aui_cost.o: $(GUIDIR)/aui.c $(GUIDIR)/aui.h $(APPDIR)/logit.h c/lib/gfx/gfx.h
	@mkdir -p $(BUILD)/apps
	$(CC) $(UCFLAGS) -DAUI_COST -c $(GUIDIR)/aui.c -o $@

$(BUILD)/gallery_cost.elf: $(GUIDIR)/gallery.c $(APPDIR)/crt0.asm $(APPDIR)/logit.h \
                           $(GUIDIR)/aui.h $(BUILD)/apps/aui_cost.o $(GFX_OBJ)
	@mkdir -p $(BUILD)/apps
	$(ASM) -f elf64 $(APPDIR)/crt0.asm -o $(BUILD)/apps/gallery_cost.crt0.o
	$(CC) $(UCFLAGS) -c $(GUIDIR)/gallery.c -o $(BUILD)/apps/gallery_cost.o
	$(LD) -nostdlib -e _start -Ttext=0x4A000000 -o $@ $(BUILD)/apps/gallery_cost.crt0.o \
	    $(BUILD)/apps/gallery_cost.o $(BUILD)/apps/aui_cost.o $(GFX_OBJ)
$(BUILD)/gallery_cost.aex: $(BUILD)/gallery_cost.elf tools/mkaex.py
	python3 tools/mkaex.py $(BUILD)/gallery_cost.elf $@ 'Gallery' - 'G' 120 140 250

# ============================================================================
# G4 -- THE CONSUMERS. Does the browser's painter actually ASK the engine for
# what CSS declared?
#
#   make test-paint-gfx          the gate
#   make test-paint-gfx-negctl   THREE controls, each of which MUST fail and
#                                must redden an EXACT count
#
# Why this lives here and not beside paint_test: paint_test's link line
# deliberately carries neither css_extra.c nor css_interp.c, and `transform`,
# `box-shadow` and every gradient value have NO producer without css_extra's
# raw-declaration scan -- they are absent from the vendored LibCSS property
# table, so the cascade cannot carry them. paint_test is therefore structurally
# unable to see any of this, which is correct for what it measures. This binary
# links the whole road instead: parse -> cascade -> css_extra capture -> layout
# -> the real browser_paint.c -> the real c/lib/gfx.
#
# THE ONE SOURCE THIS ADDS over the cssdecl gate is c/lib/image/svg.c, and it
# is not optional: img_css_color() lives there and is this tree's ONE CSS
# colour evaluator (see the argument in svg.c and in css.h's XR_* comment). A
# gradient stop's colour is resolved by calling it. Writing a second colour
# parser in the painter is the failure that file names -- two evaluators for
# one question do not fail by being approximate, they fail by DISAGREEING.
.PHONY: test-paint-gfx test-paint-gfx-negctl

PAINTGFX_DIR := $(BUILD)/paintgfx
PAINTGFX_SRC := tests/unit/paint_gfx_test.c \
                c/apps/browser/layout.c c/apps/browser/browser_paint.c \
                c/apps/browser/css_engine.c c/apps/browser/css_vars.c \
                c/apps/browser/css_extra.c c/apps/browser/css_interp.c \
                c/lib/image/svg.c

# $1 = output binary, $2 = extra -D flags
define PAINTGFX_BUILD
	@mkdir -p $(PAINTGFX_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) $(2) -o $(1) \
	    $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
endef

$(PAINTGFX_DIR)/paint_gfx_test: $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call PAINTGFX_BUILD,$@,)

$(PAINTGFX_DIR)/pg_hasbg: $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call PAINTGFX_BUILD,$@,-DPAINT_NEGCTL_ROUND_HASBG)

$(PAINTGFX_DIR)/pg_noorigin: $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call PAINTGFX_BUILD,$@,-DPAINT_NEGCTL_XF_NO_ORIGIN)

$(PAINTGFX_DIR)/pg_norclip: $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call PAINTGFX_BUILD,$@,-DPAINT_NEGCTL_NO_RCLIP)

$(PAINTGFX_DIR)/pg_noclip: $(PAINTGFX_SRC) $(GFX_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
	$(call PAINTGFX_BUILD,$@,-DPAINT_NEGCTL_SHADOW_NO_CLIP)

test-paint-gfx: $(PAINTGFX_DIR)/paint_gfx_test test-paint-gfx-negctl
	@$(PAINTGFX_DIR)/paint_gfx_test

# THE CONTROLS. Four, because the four features fail independently and a
# control only covers the one it reverts. Each must redden an EXACT count: a
# control that reddens everything proves the binary was rebuilt and nothing
# else, and a control that reddens nothing is worse than no control because it
# reads like one.
#
# Each is the PLAUSIBLE wrong implementation rather than the absent one -- all
# four of them draw a perfectly good picture:
#
#   ROUND_HASBG    the `&& e->has_bg` guard exactly as it stood before this
#                  work, so a rounded border with no background falls back to
#                  the square path. Every rounded CARD still looks right,
#                  because a card has a background; only outlines lose their
#                  corners. Reddens the rounded-outline rows.
#   XF_NO_ORIGIN   transform about the box's top-left instead of its
#                  transform-origin. This is what falls out of applying the
#                  matrix to the item's coordinates directly, and every
#                  translate() is UNAFFECTED (the origin cancels) -- so a page
#                  full of translate(-50%,-50%) centring still looks perfect
#                  and every rotated or scaled element is in the wrong place.
#   SHADOW_NO_CLIP the outer shadow painted whole, letting the element's
#                  background cover what CSS says to clip. Over an opaque card
#                  the difference is invisible; on a transparent input it
#                  washes the shadow colour across the whole control.
#   NO_RCLIP       the rectangular overflow clip alone -- exactly what this
#                  painter did before path clipping had a caller. Every
#                  scroller still clips, everything still lands in the right
#                  place, and only the four corners of a ROUNDED one are
#                  square. It is also the control that proves the rows are
#                  measuring gfx_fill_mask_clipped and not merely that a
#                  clipped box paints.
test-paint-gfx-negctl: $(PAINTGFX_DIR)/pg_hasbg $(PAINTGFX_DIR)/pg_noorigin \
                       $(PAINTGFX_DIR)/pg_noclip $(PAINTGFX_DIR)/pg_norclip
	@for c in hasbg:4 noorigin:3 noclip:2 norclip:4; do \
	   b=$${c%%:*}; want=$${c##*:}; \
	   if $(PAINTGFX_DIR)/pg_$$b > $(PAINTGFX_DIR)/$$b.log 2>&1; then \
	     echo "test-paint-gfx-negctl: FAILED -- pg_$$b PASSED, so nothing in the"; \
	     echo "  suite is measuring what it reverts."; exit 1; \
	   fi; \
	   n=`grep -c '^FAIL:' $(PAINTGFX_DIR)/$$b.log`; \
	   if [ "$$n" != "$$want" ]; then \
	     echo "test-paint-gfx-negctl: FAILED -- pg_$$b reddened $$n rows, expected $$want."; \
	     echo "  Re-measure it; never adjust the number to whatever the run printed."; \
	     grep '^FAIL:' $(PAINTGFX_DIR)/$$b.log; exit 1; \
	   fi; \
	   echo "test-paint-gfx-negctl: ok -- pg_$$b reddens exactly $$n row(s):"; \
	   grep '^FAIL:' $(PAINTGFX_DIR)/$$b.log | sed 's/^/    /'; \
	 done

# Named on the suite so it runs, and its control is a prerequisite of the
# positive above so the control runs too -- the two halves of not being in
# tests/audit-stranded.baseline.
ci-host: test-paint-gfx
