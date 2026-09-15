# Actual CSS -> layout text/background geometry; the root includes this fragment.
GENERATED_DIR := $(BUILD)/generated-content
GENERATED_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/generated_content_test.c
GENERATED_DEPS = $(GENERATED_SRC) $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout.h $(BUILD)/libcss_host.a
.PHONY: test-generated-content test-generated-content-negctl test-generated-content-blockify-negctl
$(GENERATED_DIR)/test: $(GENERATED_DEPS)
	@mkdir -p $(GENERATED_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(GENERATED_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(GENERATED_DIR)/negctl: $(GENERATED_DEPS)
	@mkdir -p $(GENERATED_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_NEGCTL_GENERATED -o $@ $(GENERATED_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-generated-content-negctl: $(GENERATED_DIR)/negctl
	@set +e; $(GENERATED_DIR)/negctl > $(GENERATED_DIR)/negctl.log 2>&1; rc=$$?; set -e; cat $(GENERATED_DIR)/negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: generated text reaches production display list' $(GENERATED_DIR)/negctl.log && \
	 grep -q 'FAIL: generated block participates before real block child' $(GENERATED_DIR)/negctl.log || { echo 'generated content negative control did not fail as expected'; exit 1; }
$(GENERATED_DIR)/blockify-negctl: $(GENERATED_DEPS)
	@mkdir -p $(GENERATED_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DLAYOUT_NEGCTL_FLEX_INLINE -o $@ $(GENERATED_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-generated-content-blockify-negctl: $(GENERATED_DIR)/blockify-negctl
	@set +e; $(GENERATED_DIR)/blockify-negctl > $(GENERATED_DIR)/blockify-negctl.log 2>&1; rc=$$?; set -e; cat $(GENERATED_DIR)/blockify-negctl.log; \
	 test $$rc -eq 1 && grep -q 'FAIL: ordinary inline flex children remain distinct sized items' $(GENERATED_DIR)/blockify-negctl.log || { echo 'inline item negative control did not fail as expected'; exit 1; }
test-generated-content: test-generated-content-negctl test-generated-content-blockify-negctl $(GENERATED_DIR)/test
	@$(GENERATED_DIR)/test
