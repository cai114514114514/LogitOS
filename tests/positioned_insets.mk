# Keep output private while sharing only the established host CSS archive.
POSITIONED_INSETS_DIR := $(BUILD)/site-general/layout/positioned-insets
POSITIONED_INSETS_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/positioned_insets_test.c
POSITIONED_INSETS_DEPS = $(POSITIONED_INSETS_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/gui/browser/layout/block.h c/apps/gui/browser/css/style.h c/apps/gui/browser/css/inset_math.inc c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a tests/positioned_insets.mk
.PHONY: test-positioned-insets test-positioned-insets-negctl
$(POSITIONED_INSETS_DIR)/test: $(POSITIONED_INSETS_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(POSITIONED_INSETS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/negctl: $(POSITIONED_INSETS_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_INSET_PERCENT_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(POSITIONED_INSETS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-positioned-insets-negctl: $(POSITIONED_INSETS_DIR)/negctl
	@$(POSITIONED_INSETS_DIR)/negctl > $(POSITIONED_INSETS_DIR)/negctl.log 2>&1; rc=$$?; cat $(POSITIONED_INSETS_DIR)/negctl.log; test $$rc -eq 1 && grep -q 'FAIL: fixed percentage top separates search control from navigation' $(POSITIONED_INSETS_DIR)/negctl.log
test-positioned-insets: test-positioned-insets-margin-negctl test-positioned-insets-negctl $(POSITIONED_INSETS_DIR)/test
	@$(POSITIONED_INSETS_DIR)/test

$(POSITIONED_INSETS_DIR)/margin-negctl: $(POSITIONED_INSETS_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_POSITION_MARGIN_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(POSITIONED_INSETS_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-positioned-insets-margin-negctl
test-positioned-insets-margin-negctl: $(POSITIONED_INSETS_DIR)/margin-negctl
	@$(POSITIONED_INSETS_DIR)/margin-negctl > $(POSITIONED_INSETS_DIR)/margin-negctl.log 2>&1; rc=$$?; cat $(POSITIONED_INSETS_DIR)/margin-negctl.log; test $$rc -eq 1 && grep -q 'FAIL: opposing fixed insets distribute horizontal auto margins' $(POSITIONED_INSETS_DIR)/margin-negctl.log

POSITIONED_PROJECTION_SRC = $(filter-out tests/unit/element_scroll_test.c,$(ELEMENT_SCROLL_SRC)) tests/unit/positioned_projection_test.c c/apps/gui/browser/dom/focus.c
POSITIONED_PROJECTION_DEPS = $(POSITIONED_PROJECTION_SRC) tests/unit/cssom_test.c c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/block.h c/apps/gui/browser/paint/text.inc c/apps/gui/browser/paint/element_scroll.inc $(QJS_SRC) $(BUILD)/libcss_host.a tests/positioned_insets.mk
$(POSITIONED_INSETS_DIR)/projection: $(POSITIONED_PROJECTION_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -o $@ $(POSITIONED_PROJECTION_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/projection-negctl: $(POSITIONED_PROJECTION_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) $(ELEMENT_SCROLL_FLAGS) -DLAYOUT_FIXED_SCROLL_LEGACY -o $@ $(POSITIONED_PROJECTION_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-positioned-projection test-positioned-projection-negctl
test-positioned-projection-negctl: $(POSITIONED_INSETS_DIR)/projection-negctl
	@$(POSITIONED_INSETS_DIR)/projection-negctl > $(POSITIONED_INSETS_DIR)/projection-negctl.log 2>&1; rc=$$?; cat $(POSITIONED_INSETS_DIR)/projection-negctl.log; test $$rc -eq 1 && grep -q 'FAIL fixed CSSOM rect ignores both page scroll axes' $(POSITIONED_INSETS_DIR)/projection-negctl.log && grep -q 'FAIL real painter keeps fixed background after page scroll' $(POSITIONED_INSETS_DIR)/projection-negctl.log
test-positioned-projection: test-positioned-projection-negctl $(POSITIONED_INSETS_DIR)/projection
	@$(POSITIONED_INSETS_DIR)/projection
test-positioned-insets: test-positioned-projection

INSET_MATH_SRC = $(filter-out tests/unit/positioned_insets_test.c,$(POSITIONED_INSETS_SRC)) tests/unit/inset_math_test.c
INSET_MATH_DEPS = $(POSITIONED_INSETS_DEPS) tests/unit/inset_math_test.c c/apps/gui/browser/css/extra_cascade.inc c/apps/gui/browser/css/inset_math.inc
$(POSITIONED_INSETS_DIR)/math: $(INSET_MATH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(INSET_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/math-negctl: $(INSET_MATH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DCSS_INSET_LENGTH_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(INSET_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-inset-math test-inset-math-negctl
test-inset-math-negctl: $(POSITIONED_INSETS_DIR)/math-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/math-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: calculated zero inset anchors overlay above flow' $(POSITIONED_INSETS_DIR)/math-negctl.log && \
	 grep -F 'FAIL: percent and pixels retain different bases' $(POSITIONED_INSETS_DIR)/math-negctl.log
test-inset-math: test-inset-math-negctl $(POSITIONED_INSETS_DIR)/math
	@$(POSITIONED_INSETS_DIR)/math
ci-host: test-inset-math


LOGICAL_MARGIN_SRC = $(filter-out tests/unit/positioned_insets_test.c,$(POSITIONED_INSETS_SRC)) tests/unit/logical_margin_test.c
LOGICAL_MARGIN_DEPS = $(POSITIONED_INSETS_DEPS) tests/unit/logical_margin_test.c c/apps/gui/browser/css/extra_cascade.inc c/apps/gui/browser/css/inset_math.inc c/apps/gui/browser/css/logical_margin.inc
$(POSITIONED_INSETS_DIR)/logical-margin: $(LOGICAL_MARGIN_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(LOGICAL_MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/logical-margin-negctl: $(LOGICAL_MARGIN_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DCSS_LOGICAL_MARGIN_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(LOGICAL_MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/flex-inset-height-negctl: $(LOGICAL_MARGIN_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_FLEX_INSET_HEIGHT_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(LOGICAL_MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/replaced-margin-negctl: $(LOGICAL_MARGIN_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_REPLACED_MARGIN_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(LOGICAL_MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-logical-margin test-logical-margin-negctl test-flex-inset-height-negctl
test-logical-margin-negctl: $(POSITIONED_INSETS_DIR)/logical-margin-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/logical-margin-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: logical block auto preserves both edge kinds' $(POSITIONED_INSETS_DIR)/logical-margin-negctl.log && \
	 grep -F 'FAIL: mixed logical margin retains percentage and pixel offset' $(POSITIONED_INSETS_DIR)/logical-margin-negctl.log
test-flex-inset-height-negctl: $(POSITIONED_INSETS_DIR)/flex-inset-height-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/flex-inset-height-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: opposing insets supply definite flex cross height' $(POSITIONED_INSETS_DIR)/flex-inset-height-negctl.log && \
	 grep -F 'FAIL: inset-sized column enters definite main-axis solver' $(POSITIONED_INSETS_DIR)/flex-inset-height-negctl.log
.PHONY: test-replaced-margin-negctl
test-replaced-margin-negctl: $(POSITIONED_INSETS_DIR)/replaced-margin-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/replaced-margin-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: block controls and images consume auto margins after sizing' $(POSITIONED_INSETS_DIR)/replaced-margin-negctl.log
test-logical-margin: test-logical-margin-negctl test-flex-inset-height-negctl test-replaced-margin-negctl $(POSITIONED_INSETS_DIR)/logical-margin
	@$(POSITIONED_INSETS_DIR)/logical-margin
ci-host: test-logical-margin

ABSOLUTE_AUTO_WIDTH_SRC = $(filter-out tests/unit/positioned_insets_test.c,$(POSITIONED_INSETS_SRC)) tests/unit/absolute_auto_width_test.c
ABSOLUTE_AUTO_WIDTH_DEPS = $(LOGICAL_MARGIN_DEPS) tests/unit/absolute_auto_width_test.c
$(POSITIONED_INSETS_DIR)/auto-width: $(ABSOLUTE_AUTO_WIDTH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_WIDTH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/auto-width-negctl: $(ABSOLUTE_AUTO_WIDTH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_POSITION_AUTO_WIDTH_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_WIDTH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-absolute-auto-width test-absolute-auto-width-negctl
test-absolute-auto-width-negctl: $(POSITIONED_INSETS_DIR)/auto-width-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/auto-width-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: one-inset auto box uses max-content not containing width' $(POSITIONED_INSETS_DIR)/auto-width-negctl.log && \
	 grep -F 'FAIL: positioned auto control retains the ordinary intrinsic width' $(POSITIONED_INSETS_DIR)/auto-width-negctl.log
test-absolute-auto-width: test-absolute-auto-width-negctl $(POSITIONED_INSETS_DIR)/auto-width
	@$(POSITIONED_INSETS_DIR)/auto-width
ci-host: test-absolute-auto-width

# Native gate takes immutable images explicitly; the old image must reach the
# geometry assertion after a real click, not merely fail to boot or load.
# These are coordinates/interaction checks, not host timing measurements.
LOGICAL_MARGIN_GUEST_ISO ?=
LOGICAL_MARGIN_GUEST_DISK ?=
LOGICAL_MARGIN_GUEST_NEG_DISK ?=

# Real decoded image dimensions and two disjoint native click targets. The
# baseline must reach the specific geometry assertion; an apparatus/boot
# failure cannot become a successful negative control.
AUTO_WIDTH_GUEST_ISO ?=
AUTO_WIDTH_GUEST_DISK ?=
AUTO_WIDTH_GUEST_NEG_DISK ?=
