# One outer atomic box path, with each real inner formatting context.
ATOMIC_DIR := $(BUILD)/atomic-inline
ATOMIC_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/atomic_inline_test.c
ATOMIC_DEPS = $(ATOMIC_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout_grid.c c/apps/browser/layout_grid.h $(BUILD)/libcss_host.a
.PHONY: test-atomic-inline test-atomic-inline-negctl
$(ATOMIC_DIR)/test: $(ATOMIC_DEPS)
	@mkdir -p $(ATOMIC_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(ATOMIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(ATOMIC_DIR)/negctl: $(ATOMIC_DEPS)
	@mkdir -p $(ATOMIC_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_ATOMIC_OUTER -o $@ $(ATOMIC_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-atomic-inline-negctl: $(ATOMIC_DIR)/negctl
	@set +e; $(ATOMIC_DIR)/negctl > $(ATOMIC_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(ATOMIC_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: atomic box remains on preceding text line' $(ATOMIC_DIR)/negctl.log && \
	 grep -q 'FAIL: atomic auto width uses inner formatting context' $(ATOMIC_DIR)/negctl.log || { echo 'atomic inline negative control did not fail as expected'; exit 1; }
test-atomic-inline: test-atomic-inline-negctl $(ATOMIC_DIR)/test
	@$(ATOMIC_DIR)/test
