# Reuse the real DOM/CSS/layout consumer. Link the parser directly before the
# archive so the legacy control replaces exactly this TU, with no second
# hand-maintained LibCSS source list and no edits to a shared archive.
FLEX_PARSER_DIR = $(BUILD)/site-general/continue/flex-parser
FLEX_PARSER_ARCHIVE ?= $(BUILD)/libcss_host.a
FLEX_PARSER_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/flex_parser_test.c third_party/css/libcss/src/parse/properties/flex.c
FLEX_PARSER_DEPS = $(FLEX_PARSER_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/browser/css.h c/apps/browser/layout.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(FLEX_PARSER_ARCHIVE) tests/flex_parser.mk
.PHONY: test-flex-parser test-flex-parser-negctl test-flex-parser-calc-audit
$(FLEX_PARSER_DIR)/test: $(FLEX_PARSER_DEPS)
	@mkdir -p $(FLEX_PARSER_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_PARSER_SRC) $(HTML_PARSER_SRC) $(FLEX_PARSER_ARCHIVE) -lm
$(FLEX_PARSER_DIR)/legacy: $(FLEX_PARSER_DEPS)
	@mkdir -p $(FLEX_PARSER_DIR)
	$(CC) -O2 -w -DCSS_FLEX_SHORTHAND_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_PARSER_SRC) $(HTML_PARSER_SRC) $(FLEX_PARSER_ARCHIVE) -lm
test-flex-parser-negctl: $(FLEX_PARSER_DIR)/legacy
	@rc=0; $(FLEX_PARSER_DIR)/legacy > $(FLEX_PARSER_DIR)/legacy.log 2>&1 || rc=$$?; cat $(FLEX_PARSER_DIR)/legacy.log; \
	 test $$rc -eq 1 && test "$$(grep -c '  FAIL:' $(FLEX_PARSER_DIR)/legacy.log)" -eq 18 && \
	 grep -q '^flex-parser: 40 checks, 18 failures$$' $(FLEX_PARSER_DIR)/legacy.log && \
	 grep -q 'FAIL: flex:1 1 0%' $(FLEX_PARSER_DIR)/legacy.log && \
	 grep -q 'FAIL: flex:1 0 --' $(FLEX_PARSER_DIR)/legacy.log && \
	 grep -q 'FAIL: three-part shorthand reaches real first flex slot' $(FLEX_PARSER_DIR)/legacy.log
test-flex-parser: test-flex-parser-negctl $(FLEX_PARSER_DIR)/test
	@$(FLEX_PARSER_DIR)/test > $(FLEX_PARSER_DIR)/current.log 2>&1; rc=$$?; cat $(FLEX_PARSER_DIR)/current.log; exit $$rc
# Deliberately RED until the existing longhand CALC cascade TODOs are built.
# This named audit is not a prerequisite: it records a distinct unsupported
# consumer, and must never be inverted into an expected-failure green gate.
test-flex-parser-calc-audit: $(FLEX_PARSER_DIR)/test
	@$(FLEX_PARSER_DIR)/test --calc-audit
