# canvas-ext: save_layer group opacity, image/sprite wrappers, stack depth, samples.
CEXT_DEPS := tests/unit/openlogit_canvas_layer_test.c $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/canvas_ext_test: $(CEXT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/canvas_ext_neg: $(CEXT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DOL_CANVAS_LAYER_PER_CHILD $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/canvas_ext_sanitize: $(CEXT_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-canvas-ext-negctl: $(BUILD)/canvas_ext_neg
	@rc=0; $< > $(BUILD)/canvas-ext-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL overlap$$' $(BUILD)/canvas-ext-neg.log
test-canvas-ext: test-canvas-ext-negctl $(BUILD)/canvas_ext_test $(BUILD)/canvas_ext_sanitize
	$(BUILD)/canvas_ext_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/canvas_ext_sanitize
ci-host: test-canvas-ext
.PHONY: test-canvas-ext test-canvas-ext-negctl
