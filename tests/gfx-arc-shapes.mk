# tests/gfx-arc-shapes.mk -- arcs, SVG endpoint arcs, ring sectors, elliptical
# and smoothed rounded rects, gfx_atan2 (c/lib/gfx/geometry/gfx_arc.c).
#
# The control is -DGFX_ARC_NEGCTL: arcs are built from 180 degree cubic pieces
# instead of at most 90. Everything still draws and looks like an arc, but the
# radial error of a single 180 degree cubic is ~1.8% of r, so the "arc-radius"
# budget must fail -- and ONLY for that reason: the control also fails if the
# log lacks that line or the binary dies without reaching the checks.

.PHONY: test-gfx-arc-shapes test-gfx-arc-shapes-negctl

GARC_CF = -O1 -g -Wall -Wextra $(GFX_INC)

$(BUILD)/gfx_arc_test: tests/unit/gfx_arc_test.c $(GFX_SRC) $(GFX_HEADERS)
	@mkdir -p $(BUILD)
	$(CC) $(GARC_CF) -o $@ tests/unit/gfx_arc_test.c $(GFX_SRC) -lm

test-gfx-arc-shapes-negctl:
	@mkdir -p $(BUILD)
	$(CC) $(GARC_CF) -DGFX_ARC_NEGCTL -o $(BUILD)/gfx_arc_negctl \
	    tests/unit/gfx_arc_test.c $(GFX_SRC) -lm
	@echo "--- negative control: the SAME assertions with 180-degree cubic pieces ---"
	@if $(BUILD)/gfx_arc_negctl > $(BUILD)/gfx_arc_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: 180-degree cubics met the radial budget"; \
	    exit 1; \
	elif ! grep -q '^FAIL arc-radius' $(BUILD)/gfx_arc_negctl.log \
	     || ! grep -q '^[0-9]* checks, ' $(BUILD)/gfx_arc_negctl.log; then \
	    cat $(BUILD)/gfx_arc_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL arc-radius' line"; \
	    exit 1; \
	else \
	    grep '^FAIL arc-radius' $(BUILD)/gfx_arc_negctl.log; \
	    echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/gfx_arc_negctl.log) assertion(s) fail with 180-degree pieces"; \
	fi

test-gfx-arc-shapes: test-gfx-arc-shapes-negctl $(BUILD)/gfx_arc_test
	$(BUILD)/gfx_arc_test

ci-host: test-gfx-arc-shapes
