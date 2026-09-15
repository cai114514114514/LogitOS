# Caret and selection consume the same advances as the actual spaced painter.
# The control restores only raw prefix metrics; painting keeps its spacing.
CARET_ADVANCE_SRC = $(filter-out tests/unit/text_wiring_test.c,$(TEXT_WIRING_SRC)) tests/unit/caret_advance_test.c
CARET_ADVANCE_DEPS = $(TEXT_WIRING_DEPS) tests/unit/text_wiring_test.c c/apps/browser/browser_paint.h
CARET_ADVANCE_DIR = $(BUILD)/site-general/caret
.PHONY: test-caret-advance test-caret-advance-negctl test-caret-long-negctl
$(CARET_ADVANCE_DIR)/caret_advance_test: $(CARET_ADVANCE_SRC) $(CARET_ADVANCE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CARET_ADVANCE_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(CARET_ADVANCE_SRC) $(BUILD)/libcss_host.a -lm
test-caret-advance-negctl: $(CARET_ADVANCE_SRC) $(CARET_ADVANCE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CARET_ADVANCE_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DBROWSER_CARET_RAW_METRICS -o $(CARET_ADVANCE_DIR)/caret_advance_raw $(CARET_ADVANCE_SRC) $(BUILD)/libcss_host.a -lm
	@rc=0; $(CARET_ADVANCE_DIR)/caret_advance_raw > $(CARET_ADVANCE_DIR)/advance_raw.log 2>&1 || rc=$$?; cat $(CARET_ADVANCE_DIR)/advance_raw.log; \
	 test "$$rc" -eq 1 && grep -q 'FAIL: caret prefix equals actual painted segment position' $(CARET_ADVANCE_DIR)/advance_raw.log
test-caret-long-negctl: $(CARET_ADVANCE_SRC) $(CARET_ADVANCE_DEPS) $(BUILD)/libcss_host.a
	@mkdir -p $(CARET_ADVANCE_DIR)
	@$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DBROWSER_LONG_TEXT_RAW_RUNS -o $(CARET_ADVANCE_DIR)/caret_long_raw $(CARET_ADVANCE_SRC) $(BUILD)/libcss_host.a -lm
	@rc=0; $(CARET_ADVANCE_DIR)/caret_long_raw > $(CARET_ADVANCE_DIR)/long_raw.log 2>&1 || rc=$$?; cat $(CARET_ADVANCE_DIR)/long_raw.log; \
	 test "$$rc" -eq 1 && \
	 grep -q 'FAIL: long ordinary run keeps a nonzero layout measurement' $(CARET_ADVANCE_DIR)/long_raw.log && \
	 grep -q 'FAIL: long ordinary run reaches native drawing in bounded complete calls' $(CARET_ADVANCE_DIR)/long_raw.log
test-caret-advance: test-caret-advance-negctl test-caret-long-negctl $(CARET_ADVANCE_DIR)/caret_advance_test
	@rc=0; $(CARET_ADVANCE_DIR)/caret_advance_test > $(CARET_ADVANCE_DIR)/advance_current.log 2>&1 || rc=$$?; cat $(CARET_ADVANCE_DIR)/advance_current.log; exit $$rc
ci-host: test-caret-advance
