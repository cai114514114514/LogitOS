LAYOUT_CAP_DIR = $(BUILD)/layout-capacity
LAYOUT_CAP_SRC = tests/unit/layout_capacity_test.c $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC))
LAYOUT_CAP_DEP = $(LAYOUT_CAP_SRC) tests/unit/layout_box_test.c c/apps/browser/layout.h $(wildcard c/apps/browser/layout*.inc) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a
$(LAYOUT_CAP_DIR)/current: $(LAYOUT_CAP_DEP)
	@mkdir -p $(LAYOUT_CAP_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LAYOUT_CAP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(LAYOUT_CAP_DIR)/old: $(LAYOUT_CAP_DEP)
	@mkdir -p $(LAYOUT_CAP_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_FIXED_CAPACITY_LEGACY -o $@ $(LAYOUT_CAP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(LAYOUT_CAP_DIR)/san: $(LAYOUT_CAP_DEP)
	@mkdir -p $(LAYOUT_CAP_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined $(BTEST_INC) $(CSS_INC) -o $@ $(LAYOUT_CAP_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-layout-capacity test-layout-capacity-negctl test-layout-capacity-san
test-layout-capacity-negctl: $(LAYOUT_CAP_DIR)/old
	@rc=0; $< > $(LAYOUT_CAP_DIR)/old.log 2>&1 || rc=$$?; cat $(LAYOUT_CAP_DIR)/old.log; test $$rc -eq 1 && grep -q 'FAIL: long text tail retained' $(LAYOUT_CAP_DIR)/old.log && grep -q 'FAIL: inkless tail box retains' $(LAYOUT_CAP_DIR)/old.log
test-layout-capacity: test-layout-capacity-negctl $(LAYOUT_CAP_DIR)/current
	@$(LAYOUT_CAP_DIR)/current
test-layout-capacity-san: test-layout-capacity $(LAYOUT_CAP_DIR)/san
	@ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 $(LAYOUT_CAP_DIR)/san
ci-host: test-layout-capacity
