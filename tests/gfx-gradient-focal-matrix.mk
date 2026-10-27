# tests/gfx-gradient-focal-matrix.mk -- gradient paint matrix (a canvas fill under a CTM
# carries it as pmat), the two-circle radial and the 4x4 Bayer dither
# (c/lib/gfx/raster/gfx_gradient.c, gfx_paint.c, canvas/state.c).
#
# The control is -DGFX_GRADIENT_IGNORE_PMAT: pmat is no longer applied, so a gradient on a
# rotated / scaled fill stays axis aligned in device space and the 'isoline' checks must
# fail -- and ONLY those: the control also fails if no 'FAIL isoline' line is logged, if any
# other check fails, or if the binary dies before the summary.

.PHONY: test-gfx-gradient-focal-matrix test-gfx-gradient-focal-matrix-negctl

GGF_SRC := tests/unit/gfx_focal_test.c $(GFX_SRC)
GGF_CF  := -O1 -g -Wall -Wextra $(GFX_INC)
GGF_DEPS := $(GGF_SRC) $(GFX_HEADERS)

$(BUILD)/gfx_focal_test: $(GGF_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GGF_CF) -o $@ $(GGF_SRC) -lm

$(BUILD)/gfx_focal_negctl: $(GGF_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(GGF_CF) -DGFX_GRADIENT_IGNORE_PMAT -o $@ $(GGF_SRC) -lm

test-gfx-gradient-focal-matrix-negctl: $(BUILD)/gfx_focal_negctl
	@echo "--- negative control: the SAME assertions with the paint matrix ignored ---"
	@if $(BUILD)/gfx_focal_negctl > $(BUILD)/gfx_focal_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: an ignored pmat passed the isoline checks"; \
	    exit 1; \
	elif ! grep -q '^FAIL isoline' $(BUILD)/gfx_focal_negctl.log \
	     || ! grep -q '^[0-9]* checks, ' $(BUILD)/gfx_focal_negctl.log; then \
	    cat $(BUILD)/gfx_focal_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL isoline' line"; \
	    exit 1; \
	elif grep '^FAIL' $(BUILD)/gfx_focal_negctl.log | grep -qv '^FAIL isoline'; then \
	    cat $(BUILD)/gfx_focal_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check other than isoline failed"; \
	    exit 1; \
	else \
	    grep -m1 '^FAIL isoline' $(BUILD)/gfx_focal_negctl.log; \
	    echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/gfx_focal_negctl.log) isoline assertion(s) fail with pmat ignored"; \
	fi

test-gfx-gradient-focal-matrix: test-gfx-gradient-focal-matrix-negctl $(BUILD)/gfx_focal_test
	$(BUILD)/gfx_focal_test

ci-host: test-gfx-gradient-focal-matrix
