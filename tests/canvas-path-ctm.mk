# canvas-path-ctm: ol_canvas_path_begin flattens under the CTM; one matrix rounding rule.
CPC_DEPS := tests/unit/openlogit_canvas_ctm_test.c $(GFX_SRC) $(GFX_HEADERS)
$(BUILD)/canvas_path_ctm_test: $(CPC_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra $(GFX_INC) $< $(GFX_SRC) -lm -o $@
$(BUILD)/canvas_path_ctm_neg: $(CPC_DEPS)
	@mkdir -p $(dir $@)
	$(CC) -O2 -Wall -Wextra -DOL_CANVAS_POST_FLATTEN $(GFX_INC) $< $(GFX_SRC) -lm -o $@
test-canvas-path-ctm-negctl: $(BUILD)/canvas_path_ctm_neg
	@rc=0; $< > $(BUILD)/canvas-path-ctm-neg.log 2>&1 || rc=$$?; test $$rc -eq 1 && grep -q '^FAIL deviation$$' $(BUILD)/canvas-path-ctm-neg.log
test-canvas-path-ctm: test-canvas-path-ctm-negctl $(BUILD)/canvas_path_ctm_test
	$(BUILD)/canvas_path_ctm_test
ci-host: test-canvas-path-ctm
.PHONY: test-canvas-path-ctm test-canvas-path-ctm-negctl
