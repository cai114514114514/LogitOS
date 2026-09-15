# Finite reflow cache acceptance: real raster pixels, not third-party input.
SVG_REFLOW_DIR = $(BUILD)/svg-reflow-cache
SVG_REFLOW_SRC = $(filter-out tests/unit/svg_dom_paint_test.c c/lib/image/img.c,$(SVG_DOM_SRC)) tests/unit/svg_reflow_cache_test.c
SVG_REFLOW_DEP = $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC) c/lib/image/img.c c/apps/browser/svg_reflow_cache.inc c/apps/browser/svg_dom_paint.inc $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
$(SVG_REFLOW_DIR)/current: $(SVG_REFLOW_DEP)
	@mkdir -p $(SVG_REFLOW_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_REFLOW_DIR)/old: $(SVG_REFLOW_DEP)
	@mkdir -p $(SVG_REFLOW_DIR)
	@$(CC) -O2 -w -DLAYOUT_SVG_NO_REFLOW_CACHE $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_REFLOW_DIR)/small: $(SVG_REFLOW_DEP)
	@mkdir -p $(SVG_REFLOW_DIR)
	@$(CC) -O2 -w -DSVG_REFLOW_MAX=2 $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_REFLOW_DIR)/key-bound: $(SVG_REFLOW_DEP)
	@mkdir -p $(SVG_REFLOW_DIR)
	@$(CC) -O2 -w -DSVG_REFLOW_KEYS=1 -DSVG_REFLOW_PASSIVE_KEYS=1 $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(SVG_REFLOW_DIR)/san: $(SVG_REFLOW_DEP)
	@mkdir -p $(SVG_REFLOW_DIR)
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -Ic/kernel/mm -o $@ $(sort $(SVG_REFLOW_SRC) $(HTML_PARSER_SRC)) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
.PHONY: test-svg-reflow-cache test-svg-reflow-cache-negctl test-svg-reflow-cache-key-bound test-svg-reflow-cache-san
test-svg-reflow-cache-negctl: $(SVG_REFLOW_DIR)/old
	@rc=0; $< > $(SVG_REFLOW_DIR)/old.log 2>&1 || rc=$$?; test $$rc -eq 1
	@python3 tests/unit/svg_reflow_cache_check.py $(SVG_REFLOW_DIR)/old.log
test-svg-reflow-cache-key-bound: $(SVG_REFLOW_DIR)/key-bound
	@rc=0; $< > $(SVG_REFLOW_DIR)/key-bound.log 2>&1 || rc=$$?; test $$rc -eq 1
	@python3 tests/unit/svg_reflow_cache_check.py $(SVG_REFLOW_DIR)/key-bound.log
test-svg-reflow-cache: test-svg-reflow-cache-negctl test-svg-reflow-cache-key-bound $(SVG_REFLOW_DIR)/current $(SVG_REFLOW_DIR)/small
	@$(SVG_REFLOW_DIR)/current
	@$(SVG_REFLOW_DIR)/small
test-svg-reflow-cache-san: test-svg-reflow-cache $(SVG_REFLOW_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(SVG_REFLOW_DIR)/san

-include tests/svg_raster_size.mk
ci-host: test-svg-reflow-cache
