# Inline SVG must rasterise at the used CSS box, rather than allocating the
# source width/height (often a multi-megapixel icon) and shrinking it in paint.
SVG_RASTER_DIR := $(BUILD)/svg-raster-size
SVG_RASTER_SRC := tests/unit/layout_svg_test.c c/apps/browser/layout.c \
                  c/apps/browser/layout_text.c c/apps/browser/css_engine.c \
                  c/apps/browser/css_vars.c $(HTML_PARSER_SRC) $(IMG_HOST_SRC)
SVG_RASTER_DEP := $(SVG_RASTER_SRC) c/apps/browser/svg_dom_paint.inc \
                  c/apps/browser/svg_reflow_cache.inc $(BUILD)/libcss_host.a \
                  $(RUST_LIB_HOST)
SVG_RASTER_INC := $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm

.PHONY: test-svg-raster-size-negctl test-svg-raster-size test-svg-raster-size-asan
$(SVG_RASTER_DIR)/current: $(SVG_RASTER_DEP)
	@mkdir -p $(SVG_RASTER_DIR)
	@$(CC) -O2 -w $(SVG_RASTER_INC) -o $@ $(SVG_RASTER_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(SVG_RASTER_DIR)/source-sized: $(SVG_RASTER_DEP)
	@mkdir -p $(SVG_RASTER_DIR)
	@$(CC) -O2 -w -DLAYOUT_SVG_SOURCE_RASTER $(SVG_RASTER_INC) -o $@ \
	    $(SVG_RASTER_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

$(SVG_RASTER_DIR)/asan: $(SVG_RASTER_DEP)
	@mkdir -p $(SVG_RASTER_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer \
	    $(SVG_RASTER_INC) -o $@ $(SVG_RASTER_SRC) \
	    $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm

test-svg-raster-size-negctl: $(SVG_RASTER_DIR)/source-sized
	@rc=0; $< > $(SVG_RASTER_DIR)/source-sized.log 2>&1 || rc=$$?; \
	 cat $(SVG_RASTER_DIR)/source-sized.log; \
	 test $$rc -eq 1 && test `grep -c '^FAIL:' $(SVG_RASTER_DIR)/source-sized.log` -eq 2 && \
	 grep -q 'FAIL: CSS-sized SVG raster avoids the 2048px source bitmap' $(SVG_RASTER_DIR)/source-sized.log && \
	 grep -q 'FAIL: enormous viewBox still rasterizes only the final CSS size' $(SVG_RASTER_DIR)/source-sized.log

test-svg-raster-size: test-svg-raster-size-negctl $(SVG_RASTER_DIR)/current
	@$(SVG_RASTER_DIR)/current

test-svg-raster-size-asan: test-svg-raster-size $(SVG_RASTER_DIR)/asan
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(SVG_RASTER_DIR)/asan

ci-host: test-svg-raster-size
