# CSS dependency assembly plus actual DOM/LibCSS geometry. Sources derive from
# the already-wired layout gate; no copied browser translation-unit inventory.
CIMPORT_DIR := $(BUILD)/css-import
CIMPORT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_import_test.c c/apps/gui/browser/css/import.c c/net/http/url.c
CIMPORT_DEPS = $(CIMPORT_SRC) $(HTML_PARSER_SRC) c/apps/gui/browser/css/import.h c/apps/gui/browser/css/style.h c/apps/gui/browser/layout/flex.c c/apps/gui/browser/layout/grid.c $(BUILD)/libcss_host.a
$(CIMPORT_DIR)/test: $(CIMPORT_DEPS)
	@mkdir -p $(CIMPORT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(CIMPORT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(CIMPORT_DIR)/negctl: $(CIMPORT_DEPS)
	@mkdir -p $(CIMPORT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_IMPORT_NEGCTL_DROP -o $@ $(CIMPORT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm

