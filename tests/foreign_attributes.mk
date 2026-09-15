FOREIGN_ATTR_DIR = $(BUILD)/foreign-attributes
FOREIGN_ATTR_SRC = $(filter-out tests/unit/dom_iface_test.c,$(DOMIFACE_SRC)) tests/unit/foreign_attribute_test.c
FOREIGN_ATTR_DEPS = $(FOREIGN_ATTR_SRC) $(HTML_PARSER_SRC) tests/unit/dom_id_test.c c/apps/browser/dom.h c/apps/browser/js_dom_iface.inc $(BUILD)/libcss_host.a $(RUST_LIB_HOST)
.PHONY: test-foreign-attributes test-foreign-attributes-negctl
$(FOREIGN_ATTR_DIR)/current: $(FOREIGN_ATTR_DEPS)
	@mkdir -p $(FOREIGN_ATTR_DIR)
	@$(CC) -O2 -w $(DOMIFACE_CF) -o $@ $(FOREIGN_ATTR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
$(FOREIGN_ATTR_DIR)/old: $(FOREIGN_ATTR_DEPS)
	@mkdir -p $(FOREIGN_ATTR_DIR)
	@$(CC) -O2 -w -DDOM_FOREIGN_ATTR_LEGACY $(DOMIFACE_CF) -o $@ $(FOREIGN_ATTR_SRC) $(HTML_PARSER_SRC) $(QJS_SRC) $(BUILD)/libcss_host.a $(RUST_LIB_HOST) -lm
test-foreign-attributes-negctl: $(FOREIGN_ATTR_DIR)/old
	@rc=0; $(FOREIGN_ATTR_DIR)/old > $(FOREIGN_ATTR_DIR)/old.log 2>&1 || rc=$$?; cat $(FOREIGN_ATTR_DIR)/old.log; test $$rc -eq 1 && grep -q 'FAIL native parsed SVG viewBox is readable' $(FOREIGN_ATTR_DIR)/old.log && grep -q 'FAIL JS SVG setAttribute updates existing viewBox' $(FOREIGN_ATTR_DIR)/old.log
test-foreign-attributes: test-foreign-attributes-negctl $(FOREIGN_ATTR_DIR)/current
	@$(FOREIGN_ATTR_DIR)/current
ci-host: test-foreign-attributes
