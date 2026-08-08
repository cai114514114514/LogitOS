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
#   make test-gfx          both host suites (seconds, no QEMU)
#   make test-gfx-negctl   the same suite with antialiasing compiled out; it
#                          MUST fail, and the target succeeds when it does
#   make bench-gfx         what the engine costs, per primitive, on the host
#   make bench-gfx-frame   what ONE APP FRAME costs on the machine, at three
#                          resolutions, with gui_clear and text separated out
# ============================================================================
.PHONY: test-gfx test-gfx-raster test-gfx-paint test-gfx-negctl bench-gfx bench-gfx-frame

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

test-gfx: test-gfx-raster test-gfx-paint

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

# Per-primitive cost on the host. Not a substitute for bench-gfx-frame -- a host
# core is not the guest under TCG -- but it is where the engine's OWN share of a
# frame is separable from syscalls, and it is the number that moves when the
# rasterizer changes.
bench-gfx: $(BUILD)/gfx_bench
	$(BUILD)/gfx_bench

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
