# Keep output private while sharing only the established host CSS archive.
POSITIONED_INSETS_DIR := $(BUILD)/site-general/layout/positioned-insets
POSITIONED_INSETS_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/positioned_insets_test.c
POSITIONED_INSETS_DEPS = $(POSITIONED_INSETS_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/layout.h c/apps/browser/css.h c/apps/browser/css_inset_math.inc c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a tests/positioned_insets.mk
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

POSITIONED_PROJECTION_SRC = $(filter-out tests/unit/element_scroll_test.c,$(ELEMENT_SCROLL_SRC)) tests/unit/positioned_projection_test.c c/apps/browser/focus.c
POSITIONED_PROJECTION_DEPS = $(POSITIONED_PROJECTION_SRC) tests/unit/cssom_test.c c/apps/browser/css.h c/apps/browser/layout.h c/apps/browser/text_paint_wiring.inc c/apps/browser/element_scroll_wiring.inc $(QJS_SRC) $(BUILD)/libcss_host.a tests/positioned_insets.mk
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
INSET_MATH_DEPS = $(POSITIONED_INSETS_DEPS) tests/unit/inset_math_test.c c/apps/browser/css_extra_cascade.inc c/apps/browser/css_inset_math.inc
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

.PHONY: test-inset-math-asan
test-inset-math-asan: test-inset-math-negctl
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(POSITIONED_INSETS_DIR)/math-asan $(INSET_MATH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(POSITIONED_INSETS_DIR)/math-asan

LOGICAL_MARGIN_SRC = $(filter-out tests/unit/positioned_insets_test.c,$(POSITIONED_INSETS_SRC)) tests/unit/logical_margin_test.c
LOGICAL_MARGIN_DEPS = $(POSITIONED_INSETS_DEPS) tests/unit/logical_margin_test.c c/apps/browser/css_extra_cascade.inc c/apps/browser/css_inset_math.inc c/apps/browser/css_logical_margin.inc
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
.PHONY: test-logical-margin test-logical-margin-negctl test-flex-inset-height-negctl test-logical-margin-asan
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
test-logical-margin-asan: test-logical-margin-negctl test-flex-inset-height-negctl test-replaced-margin-negctl
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(POSITIONED_INSETS_DIR)/logical-margin-asan $(LOGICAL_MARGIN_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(POSITIONED_INSETS_DIR)/logical-margin-asan
ci-host: test-logical-margin

ABSOLUTE_AUTO_WIDTH_SRC = $(filter-out tests/unit/positioned_insets_test.c,$(POSITIONED_INSETS_SRC)) tests/unit/absolute_auto_width_test.c
ABSOLUTE_AUTO_WIDTH_DEPS = $(LOGICAL_MARGIN_DEPS) tests/unit/absolute_auto_width_test.c
$(POSITIONED_INSETS_DIR)/auto-width: $(ABSOLUTE_AUTO_WIDTH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_WIDTH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(POSITIONED_INSETS_DIR)/auto-width-negctl: $(ABSOLUTE_AUTO_WIDTH_DEPS)
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@$(CC) -O2 -w -DLAYOUT_POSITION_AUTO_WIDTH_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(ABSOLUTE_AUTO_WIDTH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
.PHONY: test-absolute-auto-width test-absolute-auto-width-negctl test-absolute-auto-width-asan
test-absolute-auto-width-negctl: $(POSITIONED_INSETS_DIR)/auto-width-negctl
	@rc=0; $< > $(POSITIONED_INSETS_DIR)/auto-width-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'FAIL: one-inset auto box uses max-content not containing width' $(POSITIONED_INSETS_DIR)/auto-width-negctl.log && \
	 grep -F 'FAIL: positioned auto control retains the ordinary intrinsic width' $(POSITIONED_INSETS_DIR)/auto-width-negctl.log
test-absolute-auto-width: test-absolute-auto-width-negctl $(POSITIONED_INSETS_DIR)/auto-width
	@$(POSITIONED_INSETS_DIR)/auto-width
test-absolute-auto-width-asan: test-absolute-auto-width-negctl
	@$(CC) -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer $(BTEST_INC) $(CSS_INC) -o $(POSITIONED_INSETS_DIR)/auto-width-asan $(ABSOLUTE_AUTO_WIDTH_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
	@ASAN_OPTIONS=detect_leaks=0 $(POSITIONED_INSETS_DIR)/auto-width-asan
ci-host: test-absolute-auto-width

# Native gate takes immutable images explicitly; the old image must reach the
# geometry assertion after a real click, not merely fail to boot or load.
# These are coordinates/interaction checks, not host timing measurements.
LOGICAL_MARGIN_GUEST_ISO ?=
LOGICAL_MARGIN_GUEST_DISK ?=
LOGICAL_MARGIN_GUEST_NEG_DISK ?=
.PHONY: test-logical-margin-guest test-logical-margin-guest-negctl
test-logical-margin-guest-negctl:
	@test -f "$(LOGICAL_MARGIN_GUEST_ISO)" && test -f "$(LOGICAL_MARGIN_GUEST_NEG_DISK)" || { echo 'ERROR: set LOGICAL_MARGIN_GUEST_ISO and immutable LOGICAL_MARGIN_GUEST_NEG_DISK'; exit 1; }
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@rc=0; python3 tools/perf/browser_load.py --iso "$(LOGICAL_MARGIN_GUEST_ISO)" --disk "$(LOGICAL_MARGIN_GUEST_NEG_DISK)" --out $(POSITIONED_INSETS_DIR)/logical-guest-negctl --rounds 1 --cases logical-margin > $(POSITIONED_INSETS_DIR)/logical-guest-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'AssertionError: logical margin dialog is not centered: LOGICAL-MARGIN OPEN FAIL' $(POSITIONED_INSETS_DIR)/logical-guest-negctl.log
test-logical-margin-guest: test-logical-margin-guest-negctl
	@test -f "$(LOGICAL_MARGIN_GUEST_DISK)" || { echo 'ERROR: set immutable LOGICAL_MARGIN_GUEST_DISK'; exit 1; }
	python3 tools/perf/browser_load.py --iso "$(LOGICAL_MARGIN_GUEST_ISO)" --disk "$(LOGICAL_MARGIN_GUEST_DISK)" --out $(POSITIONED_INSETS_DIR)/logical-guest --rounds 1 --cases logical-margin

# Real decoded image dimensions and two disjoint native click targets. The
# baseline must reach the specific geometry assertion; an apparatus/boot
# failure cannot become a successful negative control.
AUTO_WIDTH_GUEST_ISO ?=
AUTO_WIDTH_GUEST_DISK ?=
AUTO_WIDTH_GUEST_NEG_DISK ?=
.PHONY: test-absolute-auto-width-guest test-absolute-auto-width-guest-negctl
test-absolute-auto-width-guest-negctl:
	@test -f "$(AUTO_WIDTH_GUEST_ISO)" && test -f "$(AUTO_WIDTH_GUEST_NEG_DISK)" || { echo 'ERROR: set AUTO_WIDTH_GUEST_ISO and immutable AUTO_WIDTH_GUEST_NEG_DISK'; exit 1; }
	@mkdir -p $(POSITIONED_INSETS_DIR)
	@rc=0; python3 tools/perf/browser_load.py --iso "$(AUTO_WIDTH_GUEST_ISO)" --disk "$(AUTO_WIDTH_GUEST_NEG_DISK)" --out $(POSITIONED_INSETS_DIR)/auto-width-guest-negctl --rounds 1 --cases absolute-auto-width > $(POSITIONED_INSETS_DIR)/auto-width-guest-negctl.log 2>&1 || rc=$$?; \
	 test $$rc -eq 1 && grep -F 'AssertionError: positioned auto width geometry failed: AUTO-WIDTH FAIL' $(POSITIONED_INSETS_DIR)/auto-width-guest-negctl.log
test-absolute-auto-width-guest: test-absolute-auto-width-guest-negctl
	@test -f "$(AUTO_WIDTH_GUEST_DISK)" || { echo 'ERROR: set immutable AUTO_WIDTH_GUEST_DISK'; exit 1; }
	python3 tools/perf/browser_load.py --iso "$(AUTO_WIDTH_GUEST_ISO)" --disk "$(AUTO_WIDTH_GUEST_DISK)" --out $(POSITIONED_INSETS_DIR)/auto-width-guest --rounds 1 --cases absolute-auto-width
