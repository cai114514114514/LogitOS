# tests/ol-image-pattern.mk -- image paints in vector fills: a retained ol_image as the paint of
# ol_cmd_fill / ol_cmd_fill_ex (stale detection), CLAMP/REPEAT/REFLECT spread, and the BOX
# downscale filter (c/lib/gfx/core/ol_record.c, ol_exec.c, raster/gfx_image_sample.c).
#
# The control is -DOL_IMAGE_PAINT_REJECT (the legacy paint_ok, which refuses every image paint):
# the 'image-fill' check must fail. The control also fails if the log lacks a 'FAIL image-fill'
# line, if a check outside the image-fill family fails, or if the binary dies before its summary.

.PHONY: test-ol-image-pattern test-ol-image-pattern-negctl

OIP_SRC := tests/unit/gfx_image_pattern_test.c $(GFX_SRC)
OIP_CF  := -O1 -g -Wall -Wextra $(GFX_INC) $(HOST_INCDIRS)
OIP_DEPS := $(OIP_SRC) $(GFX_HEADERS)

$(BUILD)/ol_image_pattern_test: $(OIP_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(OIP_CF) -o $@ $(OIP_SRC) -lm

$(BUILD)/ol_image_pattern_negctl: $(OIP_DEPS)
	@mkdir -p $(BUILD)
	$(CC) $(OIP_CF) -DOL_IMAGE_PAINT_REJECT -o $@ $(OIP_SRC) -lm

test-ol-image-pattern-negctl: $(BUILD)/ol_image_pattern_negctl
	@echo "--- negative control: the SAME assertions with the legacy paint_ok ---"
	@if $(BUILD)/ol_image_pattern_negctl > $(BUILD)/ol_image_pattern_negctl.log 2>&1; then \
	    echo "NEGATIVE CONTROL FAILED: the legacy paint_ok passed the image-fill check"; \
	    exit 1; \
	elif ! grep -q '^FAIL image-fill ' $(BUILD)/ol_image_pattern_negctl.log \
	     || ! grep -q '^[0-9]* checks, ' $(BUILD)/ol_image_pattern_negctl.log; then \
	    cat $(BUILD)/ol_image_pattern_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: no 'FAIL image-fill' line"; \
	    exit 1; \
	elif grep '^FAIL' $(BUILD)/ol_image_pattern_negctl.log | grep -qv '^FAIL image-fill'; then \
	    cat $(BUILD)/ol_image_pattern_negctl.log; \
	    echo "NEGATIVE CONTROL FAILED FOR THE WRONG REASON: a check outside image-fill failed"; \
	    exit 1; \
	else \
	    grep -m1 '^FAIL image-fill ' $(BUILD)/ol_image_pattern_negctl.log; \
	    echo "negative control ok: $$(grep -c '^FAIL' $(BUILD)/ol_image_pattern_negctl.log) image-fill assertion(s) fail with the legacy paint_ok"; \
	fi

test-ol-image-pattern: test-ol-image-pattern-negctl $(BUILD)/ol_image_pattern_test
	$(BUILD)/ol_image_pattern_test

ci-host: test-ol-image-pattern
