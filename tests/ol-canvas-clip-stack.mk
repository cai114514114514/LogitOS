# ol-canvas-clip-stack: canvas rrect/path clips multiply with the saved mask.
OCCS_DEPS := tests/unit/openlogit_canvas_clip_test.c $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/ol_canvas_clip_test: $(OCCS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/ol_canvas_clip_neg: $(OCCS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DOL_CANVAS_CLIP_REPLACE $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/ol_canvas_clip_sanitize: $(OCCS_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-ol-canvas-clip-stack-negctl: $(BUILD)/ol_canvas_clip_neg
	@rc=0; $< > $(BUILD)/ol-canvas-clip-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL nested$$' $(BUILD)/ol-canvas-clip-neg.log
test-ol-canvas-clip-stack: test-ol-canvas-clip-stack-negctl $(BUILD)/ol_canvas_clip_test $(BUILD)/ol_canvas_clip_sanitize
	$(BUILD)/ol_canvas_clip_test
	ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(BUILD)/ol_canvas_clip_sanitize
ci-host: test-ol-canvas-clip-stack
.PHONY: test-ol-canvas-clip-stack test-ol-canvas-clip-stack-negctl
