# Keep output private while sharing only the established host CSS archive.
DISPLAY_CONTENTS_DIR := $(BUILD)/site-general/layout/display-contents
DISPLAY_CONTENTS_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/display_contents_test.c
DISPLAY_CONTENTS_DEPS = $(DISPLAY_CONTENTS_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/display_contents.mk
.PHONY: test-display-contents test-display-contents-negctl
$(DISPLAY_CONTENTS_DIR)/test: $(DISPLAY_CONTENTS_DEPS)
	@mkdir -p $(DISPLAY_CONTENTS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(DISPLAY_CONTENTS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(DISPLAY_CONTENTS_DIR)/negctl: $(DISPLAY_CONTENTS_DEPS)
	@mkdir -p $(DISPLAY_CONTENTS_DIR)
	@$(CC) -O2 -w -DLAYOUT_CONTENTS_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(DISPLAY_CONTENTS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-display-contents-negctl: $(DISPLAY_CONTENTS_DIR)/negctl
	@$(DISPLAY_CONTENTS_DIR)/negctl > $(DISPLAY_CONTENTS_DIR)/negctl.log 2>&1; rc=$$?; cat $(DISPLAY_CONTENTS_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: nested contents children join flex row' $(DISPLAY_CONTENTS_DIR)/negctl.log
test-display-contents: test-display-contents-negctl $(DISPLAY_CONTENTS_DIR)/test
	@$(DISPLAY_CONTENTS_DIR)/test

# The guest consumer uses CSSOM, whose inline-fragment fallback is a separate
# producer from layout_node_box. Keep that boundary in the positive gate too.
DISPLAY_CONTENTS_CSSOM_SRC = $(filter-out tests/unit/cssom_test.c,$(CSSOM_TEST_SRC)) tests/unit/display_contents_cssom_test.c $(HTML_PARSER_SRC)
DISPLAY_CONTENTS_CSSOM_DEPS = $(DISPLAY_CONTENTS_CSSOM_SRC) tests/unit/cssom_test.c $(QJS_SRC) $(BUILD)/libcss_host.a tests/display_contents.mk
DISPLAY_CONTENTS_CSSOM_FLAGS = -O2 -w $(BTEST_INC) $(CSS_INC) $(JS_INC) -DCONFIG_VERSION='"host"' -DWEBAPI_HOST
$(DISPLAY_CONTENTS_DIR)/cssom: $(DISPLAY_CONTENTS_CSSOM_DEPS)
	@mkdir -p $(DISPLAY_CONTENTS_DIR)
	@$(CC) $(DISPLAY_CONTENTS_CSSOM_FLAGS) -o $@ $(DISPLAY_CONTENTS_CSSOM_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(DISPLAY_CONTENTS_DIR)/cssom-negctl: $(DISPLAY_CONTENTS_CSSOM_DEPS)
	@mkdir -p $(DISPLAY_CONTENTS_DIR)
	@$(CC) $(DISPLAY_CONTENTS_CSSOM_FLAGS) -DCSSOM_CONTENTS_INK_UNION -o $@ $(DISPLAY_CONTENTS_CSSOM_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-display-contents-cssom test-display-contents-cssom-negctl
test-display-contents-cssom-negctl: $(DISPLAY_CONTENTS_DIR)/cssom-negctl
	@$(DISPLAY_CONTENTS_DIR)/cssom-negctl > $(DISPLAY_CONTENTS_DIR)/cssom-negctl.log 2>&1; rc=$$?; cat $(DISPLAY_CONTENTS_DIR)/cssom-negctl.log; test $$rc -eq 1 && grep -q 'FAIL contents bounding rect is zero not child union' $(DISPLAY_CONTENTS_DIR)/cssom-negctl.log
test-display-contents-cssom: test-display-contents-cssom-negctl $(DISPLAY_CONTENTS_DIR)/cssom
	@$(DISPLAY_CONTENTS_DIR)/cssom
test-display-contents: test-display-contents-cssom
