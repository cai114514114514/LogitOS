CANVAS_SDK_DEPS := tests/unit/openlogit_canvas_test.c $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/openlogit_canvas_test: $(CANVAS_SDK_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_canvas_neg: $(CANVAS_SDK_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -DOPENLOGIT_DAMAGE_OLD_DISABLED $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/openlogit_canvas_sanitize: $(CANVAS_SDK_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-openlogit-canvas-neg: $(BUILD)/openlogit_canvas_neg
	@rc=0; $< > $(BUILD)/openlogit-canvas-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && rg '^FAIL moving layer clears' $(BUILD)/openlogit-canvas-neg.log
test-openlogit-canvas: test-openlogit-canvas-neg $(BUILD)/openlogit_canvas_test $(BUILD)/openlogit_canvas_sanitize
	$(BUILD)/openlogit_canvas_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/openlogit_canvas_sanitize
test-openlogit: test-openlogit-canvas
ci-host: test-openlogit-canvas
.PHONY: test-openlogit-canvas test-openlogit-canvas-neg
