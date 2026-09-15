# CSS dependency assembly plus actual DOM/LibCSS geometry. Sources derive from
# the already-wired layout gate; no copied browser translation-unit inventory.
CIMPORT_DIR := $(BUILD)/css-import
CIMPORT_SRC = $(filter-out tests/unit/layout_box_test.c,$(LBOX_SRC)) tests/unit/css_import_test.c c/apps/browser/css_import.c c/net/http/url.c
CIMPORT_DEPS = $(CIMPORT_SRC) $(HTML_PARSER_SRC) c/apps/browser/css_import.h c/apps/browser/css.h c/apps/browser/layout_flex.c c/apps/browser/layout_grid.c $(BUILD)/libcss_host.a
.PHONY: test-css-import test-css-import-negctl
$(CIMPORT_DIR)/test: $(CIMPORT_DEPS)
	@mkdir -p $(CIMPORT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -o $@ $(CIMPORT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
$(CIMPORT_DIR)/negctl: $(CIMPORT_DEPS)
	@mkdir -p $(CIMPORT_DIR)
	@$(CC) -O2 -w $(BTEST_INC) $(CSS_INC) -DCSS_IMPORT_NEGCTL_DROP -o $@ $(CIMPORT_SRC) $(HTML_PARSER_SRC) $(BUILD)/libcss_host.a -lm
test-css-import-negctl: $(CIMPORT_DIR)/negctl
	@rc=0; $(CIMPORT_DIR)/negctl > $(CIMPORT_DIR)/negctl.log 2>&1 || rc=$$?; \
	 cat $(CIMPORT_DIR)/negctl.log; \
	 if [ "$$rc" -ne 1 ] || ! grep -Fq 'FAIL: imported rule hides Navigation' $(CIMPORT_DIR)/negctl.log; then \
	   echo "css-import: invalid negative control (exit $$rc; expected assertion exit 1 and Navigation failure)"; exit 1; \
	 fi

test-css-import: test-css-import-negctl $(CIMPORT_DIR)/test
	@$(CIMPORT_DIR)/test
