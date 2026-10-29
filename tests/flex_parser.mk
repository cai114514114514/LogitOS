# Reuse the real DOM/CSS/layout consumer. Link the parser directly before the
# archive so the legacy control replaces exactly this TU, with no second
# hand-maintained LibCSS source list and no edits to a shared archive.
FLEX_PARSER_DIR = $(BUILD)/site-general/continue/flex-parser
FLEX_PARSER_ARCHIVE ?= $(BUILD)/libcss_host.a
FLEX_PARSER_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/flex_parser_test.c third_party/css/libcss/src/parse/properties/flex.c
FLEX_PARSER_DEPS = $(FLEX_PARSER_SRC) tests/unit/intrinsic_test.c $(HTML_PARSER_SRC) c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/block.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(FLEX_PARSER_ARCHIVE) tests/flex_parser.mk
$(FLEX_PARSER_DIR)/test: $(FLEX_PARSER_DEPS)
	@mkdir -p $(FLEX_PARSER_DIR)
	$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_PARSER_SRC) $(HTML_PARSER_SRC) $(FLEX_PARSER_ARCHIVE) -lm
$(FLEX_PARSER_DIR)/legacy: $(FLEX_PARSER_DEPS)
	@mkdir -p $(FLEX_PARSER_DIR)
	$(CC) -O2 -w -DCSS_FLEX_SHORTHAND_LEGACY $(BTEST_INC) $(CSS_INC) -o $@ $(FLEX_PARSER_SRC) $(HTML_PARSER_SRC) $(FLEX_PARSER_ARCHIVE) -lm
