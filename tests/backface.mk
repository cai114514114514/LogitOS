BACKFACE_DIR = $(BUILD)/backface
BACKFACE_SRC = $(filter-out tests/unit/modal_paint_test.c,$(MODAL_PAINT_SRC)) tests/unit/backface_test.c
BACKFACE_DEPS = $(BACKFACE_SRC) tests/unit/modal_paint_test.c c/apps/browser/css.h c/apps/browser/browser_backface.inc c/apps/browser/css_extra_cascade.inc $(BUILD)/libcss_host.a
$(BACKFACE_DIR)/current: $(BACKFACE_DEPS)
	@mkdir -p $(BACKFACE_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(BACKFACE_SRC) $(BUILD)/libcss_host.a -lm
$(BACKFACE_DIR)/old: $(BACKFACE_DEPS)
	@mkdir -p $(BACKFACE_DIR)
	$(CC) -O2 -w $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -DPAINT_BACKFACE_VISIBLE_LEGACY -o $@ $(BACKFACE_SRC) $(BUILD)/libcss_host.a -lm
$(BACKFACE_DIR)/san: $(BACKFACE_DEPS)
	@mkdir -p $(BACKFACE_DIR)
	$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(PAINT_INC) $(BTEST_INC) $(CSS_INC) -o $@ $(BACKFACE_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-backface test-backface-negctl test-backface-san
test-backface-negctl: $(BACKFACE_DIR)/old
	@rc=0; $(BACKFACE_DIR)/old > $(BACKFACE_DIR)/old.log 2>&1 || rc=$$?; cat $(BACKFACE_DIR)/old.log; test $$rc -eq 1
	python3 tests/unit/backface_check.py $(BACKFACE_DIR)/old.log
test-backface: test-backface-negctl $(BACKFACE_DIR)/current
	@$(BACKFACE_DIR)/current
test-backface-san: test-backface $(BACKFACE_DIR)/san
	ASAN_OPTIONS=detect_leaks=0 $(BACKFACE_DIR)/san
ci-host: test-backface
